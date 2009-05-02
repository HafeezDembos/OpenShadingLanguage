/*****************************************************************************
 *
 *             Copyright (c) 2009 Sony Pictures Imageworks, Inc.
 *                            All rights reserved.
 *
 *  This material contains the confidential and proprietary information
 *  of Sony Pictures Imageworks, Inc. and may not be disclosed, copied or
 *  duplicated in any form, electronic or hardcopy, in whole or in part,
 *  without the express prior written consent of Sony Pictures Imageworks,
 *  Inc. This copyright notice does not imply publication.
 *
 *****************************************************************************/


#include <vector>
#include <string>
#include <fstream>
#include <cstdio>
#include <streambuf>
#ifdef __GNUC__
# include <ext/stdio_filebuf.h>
#endif
#include <cstdio>
#include <cerrno>

#include "OpenImageIO/strutil.h"
#include "OpenImageIO/dassert.h"

#include "oslcomp_pvt.h"


#define yyFlexLexer oslFlexLexer
#include "FlexLexer.h"


namespace OSL {


OSLCompiler *
OSLCompiler::create ()
{
    return new pvt::OSLCompilerImpl;
}



namespace pvt {   // OSL::pvt


OSLCompilerImpl *oslcompiler = NULL;


OSLCompilerImpl::OSLCompilerImpl ()
    : m_lexer(NULL), m_err(false), m_symtab(*this),
      m_current_typespec(TypeDesc::UNKNOWN), m_current_output(false),
      m_verbose(false), m_debug(false), m_next_temp(0), m_next_const(0),
      m_osofile(NULL), m_sourcefile(NULL), m_last_sourceline(0)
{
    initialize_globals ();
    initialize_builtin_funcs ();
}



OSLCompilerImpl::~OSLCompilerImpl ()
{
    if (m_sourcefile) {
        fclose (m_sourcefile);
        m_sourcefile = NULL;
    }
}



void
OSLCompilerImpl::error (ustring filename, int line, const char *format, ...)
{
    va_list ap;
    va_start (ap, format);
    std::string errmsg = format ? Strutil::vformat (format, ap) : "syntax error";
    if (filename.c_str())
        fprintf (stderr, "%s:%d: error: %s\n", 
                 filename.c_str(), line, errmsg.c_str());
    else
        fprintf (stderr, "error: %s\n", errmsg.c_str());

    va_end (ap);
    m_err = true;
}



void
OSLCompilerImpl::warning (ustring filename, int line, const char *format, ...)
{
    va_list ap;
    va_start (ap, format);
    std::string errmsg = format ? Strutil::vformat (format, ap) : "";
    fprintf (stderr, "%s:%d: warning: %s\n", 
             filename.c_str(), line, errmsg.c_str());
    va_end (ap);
}



bool
OSLCompilerImpl::compile (const std::string &filename,
                          const std::vector<std::string> &options)
{
    std::string cppcommand = "/usr/bin/cpp -xc -nostdinc ";

    for (size_t i = 0;  i < options.size();  ++i) {
        if (options[i] == "-v") {
            // verbose mode
            m_verbose = true;
        } else if (options[i] == "-d") {
            // debug mode
            m_debug = true;
        } else {
            // something meant for the cpp command
            cppcommand += "\"";
            cppcommand += options[i];
            cppcommand += "\" ";
        }
    }
    cppcommand += "\"";
    cppcommand += filename;
    cppcommand += "\" ";

    // std::cout << "cpp command:\n>" << cppcommand << "<\n";

    FILE *cpppipe = popen (cppcommand.c_str(), "r");

#ifdef __GNUC__
    __gnu_cxx::stdio_filebuf<char> fb (cpppipe, std::ios::in);
#else
    std::filebuf fb (cpppipe);
#endif

    if (fb.is_open()) {
        std::istream in (&fb);
        oslcompiler = this;

        // Create a lexer, parse the file, delete the lexer
        m_lexer = new oslFlexLexer (&in);
        oslparse ();
        bool parseerr = error_encountered();
        delete m_lexer;

        // All done with the input, close the files
        fb.close ();
        pclose (cpppipe);
        cpppipe = NULL;

        if (! error_encountered()) {
            oslcompiler->shader()->typecheck ();
        }

        // Print the parse tree if there were no errors
        if (m_debug) {
            oslcompiler->symtab().print ();
//            if (! parseerr)
            oslcompiler->shader()->print (std::cout);
        }

        if (! error_encountered()) {
            oslcompiler->shader()->codegen ();
        }
 
        if (! error_encountered()) {
            std::string outname = output_filename (filename);
            write_oso_file (outname);
        }

        oslcompiler = NULL;
    }

    return ! error_encountered();
}



struct GlobalTable {
    const char *name;
    TypeSpec type;
};

static GlobalTable globals[] = {
    { "P", TypeDesc::TypePoint },
    { "I", TypeDesc::TypeVector },
    { "N", TypeDesc::TypeNormal },
    { "Ng", TypeDesc::TypeNormal },
    { "u", TypeDesc::TypeFloat },
    { "v", TypeDesc::TypeFloat },
    { "dPdu", TypeDesc::TypeVector },
    { "dPdv", TypeDesc::TypeVector },
    { "L", TypeDesc::TypeVector },
    { "Cl", TypeDesc::TypeColor },
    { "Ps", TypeDesc::TypePoint },
    { "Ns", TypeDesc::TypeNormal },
    { "Pl", TypeDesc::TypePoint },
    { "Nl", TypeDesc::TypeNormal },
    { "Ci", TypeSpec (TypeDesc::TypeColor, true) },
    { "Oi", TypeDesc::TypeColor },
    { "time", TypeDesc::TypeFloat },
    { "dtime", TypeDesc::TypeFloat },
    { "dPdtime", TypeDesc::TypeVector },
    { NULL }
};


void
OSLCompilerImpl::initialize_globals ()
{
    for (int i = 0;  globals[i].name;  ++i) {
        Symbol *s = new Symbol (ustring(globals[i].name), globals[i].type,
                                SymTypeGlobal);
        symtab().insert (s);
    }
}



std::string
OSLCompilerImpl::output_filename (const std::string &inputfilename)
{
    if (m_shader && shader_decl())
        return shader_decl()->shadername().string() + ".oso";
    return std::string();
}



void
OSLCompilerImpl::write_oso_metadata (const ASTNode *metanode) const
{
    const ASTvariable_declaration *metavar = dynamic_cast<const ASTvariable_declaration *>(metanode);
    ASSERT (metavar);
    Symbol *metasym = metavar->sym();
    ASSERT (metasym);
    TypeSpec ts = metasym->typespec();
    oso ("%%meta{%s,%s,", ts.string().c_str(), metasym->name().c_str());
    const ASTNode *init = metavar->init().get();
    ASSERT (init);
    if (ts.is_string() && init->nodetype() == ASTNode::literal_node)
        oso ("\"%s\"", ((const ASTliteral *)init)->strval());
    else if (ts.is_int() && init->nodetype() == ASTNode::literal_node)
        oso ("%d", ((const ASTliteral *)init)->intval());
    else if (ts.is_float() && init->nodetype() == ASTNode::literal_node)
        oso ("%g", ((const ASTliteral *)init)->floatval());
    // FIXME -- what about type constructors?
    else {
        std::cout << "Error, don't know how to print metadata " 
                  << ts.string() << " with node type " 
                  << init->nodetypename() << "\n";
        ASSERT (0);  // FIXME
    }
    oso ("} ");
}



void
OSLCompilerImpl::write_oso_const_value (const ConstantSymbol *sym) const
{
    ASSERT (sym);
    if (sym->typespec().is_string())
        oso ("\"%s\"", sym->strval().c_str());
    else if (sym->typespec().is_int())
        oso ("%d", sym->intval());
    else if (sym->typespec().is_float())
        oso ("%g", sym->floatval());
    else {
        ASSERT (0 && "Only know how to output const vals that are single int, float, string");
    }
}



void
OSLCompilerImpl::write_oso_formal_default (const ASTvariable_declaration *node) const
{
    Symbol *sym = node->sym();
    TypeSpec type = sym->typespec();

    // FIXME -- this only works for single values or arrays made of
    // literals.  Needs to be seriously beefed up.

    for (ASTNode::ref init = node->init();  init;  init = init->next()) {
        ASTliteral *lit = dynamic_cast<ASTliteral *>(init.get());
        if (type.is_int()) {
            if (lit && lit->typespec().is_int())
                oso ("%d ", lit->intval());
            else
                oso ("0 ");  // FIXME?
        } else if (type.is_float()) {
            if (lit && lit->typespec().is_int())
                oso ("%d ", lit->intval());
            else if (lit && lit->typespec().is_float())
                oso ("%g ", lit->floatval());
            else
                oso ("0 ");  // FIXME?
        } else if (type.is_triple()) {
            float f = 0;
            if (lit && lit->typespec().is_int())
                f = lit->intval();
            else if (lit && lit->typespec().is_float())
                f = lit->floatval();
            else
                f = 0;  // FIXME?
            oso ("%g %g %g ", f, f, f);
        } else if (type.is_matrix()) {
            float f = 0;
            if (lit && lit->typespec().is_int())
                f = lit->intval();
            else if (lit && lit->typespec().is_float())
                f = lit->floatval();
            else
                f = 0;  // FIXME?
            oso ("%g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g ",
                 f, f, f, f, f, f, f, f, f, f, f, f, f, f, f, f);
        } else if (type.is_string()) {
            if (lit && lit->typespec().is_string())
                oso ("\"%s\" ", lit->strval());
            else
                oso ("\"\" ");  // FIXME?
        }
        else {
            ASSERT (0 && "help with initializer");
        }
    }
}



void
OSLCompilerImpl::write_oso_symbol (const Symbol *sym) const
{
    oso ("%s\t%s\t%s", sym->symtype_shortname(),
         sym->typespec().string().c_str(), sym->mangled().c_str());

    ASTvariable_declaration *v = NULL;
    if (sym->node())
        v = dynamic_cast<ASTvariable_declaration *>(sym->node());

    // Print default values
    if (sym->symtype() == SymTypeConst) {
        oso ("\t");
        write_oso_const_value (dynamic_cast<const ConstantSymbol *>(sym));
        oso ("\t");
    } else if (v && (sym->symtype() == SymTypeParam ||
                    sym->symtype() == SymTypeOutputParam)) {
        oso ("\t");
        write_oso_formal_default (v);
        oso ("\t");
    }

    int hints = 0;
    if (v) {
        ASSERT (v);
        for (ASTNode::ref m = v->meta();  m;  m = m->next()) {
            if (hints++ == 0)
                oso ("\t");
            write_oso_metadata (m.get());
        }
    }

    oso ("\n");
}



void
OSLCompilerImpl::write_oso_file (const std::string &outfilename)
{
    ASSERT (m_osofile == NULL);
    m_osofile = fopen (outfilename.c_str(), "w");
    if (! m_osofile) {
        error (ustring(), 0, "Could not open \"%s\"", outfilename.c_str());
        return;
    }

    // FIXME -- remove the hard-coded version!
    oso ("OpenShadingLanguage 0.0\n");
    oso ("# Compiled by oslc FIXME-VERSION\n");

    ASTshader_declaration *shaderdecl = shader_decl();
    oso ("%s %s", shaderdecl->shadertypename(), 
         shaderdecl->shadername().c_str());

    // FIXME -- output hints and metadata

    oso ("\n");

    // Output params, so they are first
    for (SymbolList::const_iterator s = symtab().symbegin();
             s != symtab().symend();  ++s) {
        if ((*s)->symtype() == SymTypeParam ||
                (*s)->symtype() == SymTypeOutputParam) {
            write_oso_symbol (*s);
        }
    }
    // Output globals, locals, temps, const
    for (SymbolList::const_iterator s = symtab().symbegin();
             s != symtab().symend();  ++s) {
        if ((*s)->symtype() == SymTypeLocal ||
                (*s)->symtype() == SymTypeTemp ||
                (*s)->symtype() == SymTypeGlobal ||
                (*s)->symtype() == SymTypeConst) {
            write_oso_symbol (*s);
        }
    }

    // FIXME -- output all opcodes
    int lastline = -1;
    ustring lastfile;
    ustring lastmethod ("___uninitialized___");
    for (IROpcodeVec::iterator op = m_ircode.begin(); op != m_ircode.end();  ++op) {
        if (lastmethod != op->method()) {
            oso ("code %s\n", op->method().c_str());
            lastmethod = op->method();
            lastfile = ustring();
            lastline = -1;
        }

        if (m_debug && op->node()) {
            ustring file = op->node()->sourcefile();
            int line = op->node()->sourceline();
            if (file != lastfile || line != lastline)
                oso ("# %s:%d\n# %s\n", file.c_str(), line,
                     retrieve_source (file, line).c_str());
        }

        // Op name
        oso ("\t%s", op->opname());

        // Register arguments
        if (op->nargs())
            oso (strlen(op->opname()) < 8 ? "\t\t" : "\t");
        for (size_t i = 0;  i < op->nargs();  ++i) {
            oso ("%s ", op->arg(i)->dealias()->mangled().c_str());
        }

        // Jump targets
        for (int i = 0;  i < IROpcode::max_jumps;  ++i)
            if (op->jump(i) >= 0)
                oso ("%d ", op->jump(i));

        // Hints
        bool firsthint = true;
        if (op->node()) {
            if (op->node()->sourcefile() != lastfile) {
                lastfile = op->node()->sourcefile();
                oso ("%c%%filename{\"%s\"}", firsthint ? '\t' : ' ', lastfile.c_str());
                firsthint = false;
            }
            if (op->node()->sourceline() != lastline) {
                lastline = op->node()->sourceline();
                oso ("%c%%line{%d}", firsthint ? '\t' : ' ', lastline);
                firsthint = false;
            }
        }
        oso ("\n");
    }
    oso ("\tend\n");

    fclose (m_osofile);
    m_osofile = NULL;
}



void
OSLCompilerImpl::oso (const char *fmt, ...) const
{
    // FIXME -- might be nice to let this save to a memory buffer, not
    // just a file.
    va_list arg_ptr;
    va_start (arg_ptr, fmt);
    vfprintf (m_osofile, fmt, arg_ptr);
    va_end (arg_ptr);
}



std::string
OSLCompilerImpl::retrieve_source (ustring filename, int line)
{
    // If we don't already have the file open, open it
    if (filename != m_last_sourcefile) {
        // If we have another file open, close that one
        if (m_sourcefile)
            fclose (m_sourcefile);
        m_last_sourcefile = filename;
        m_sourcefile = fopen (filename.c_str(), "r");
        if (! m_sourcefile) {
            m_last_sourcefile = ustring();
            return "<not found>";
        }
    }

    // If we want something *before* the last line read in the open file,
    // rewind to the beginning.
    if (m_last_sourceline > line) {
        rewind (m_sourcefile);
        m_last_sourceline = 0;
    }

    // Now read lines up to and including the file we want.
    char buf[10240];
    while (m_last_sourceline < line) {
        fgets (buf, sizeof(buf), m_sourcefile);
        ++m_last_sourceline;
    }

    // strip trailing newline
    if (buf[strlen(buf)-1] == '\n')
        buf[strlen(buf)-1] = '\0';

    return std::string (buf);
}


}; // namespace pvt
}; // namespace OSL