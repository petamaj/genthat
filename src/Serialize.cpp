#include <Rcpp.h>
#include <R.h>

#include <string>
#include <regex>

#include "utils.h"

using namespace Rcpp;
using namespace std;

// options for deparse
// from Defn.h
#define KEEPINTEGER 	1
#define SHOWATTRIBUTES 	4
#define KEEPNA			64
#define HEXNUMERIC      256
#define DIGITS16        512

extern "C" {
    SEXP Rf_deparse1(SEXP call, Rboolean abbrev, int opts);
    SEXP Rf_eval(SEXP e, SEXP rho);
}

class serialization_error : public runtime_error {
public:
    serialization_error(string details) : runtime_error("Serialization error: " + details) {}
};

class sexp_not_supported_error : public serialization_error {
public:
    sexp_not_supported_error(string sexp_type) : serialization_error("SEXP type " + sexp_type + " not supported!") {}
};

class cycle_error : public serialization_error {
public:
    cycle_error() : serialization_error("Serialized data structure contains cycle!") {}
};

static const set<string> BASE_INFIX_FUNS = {
    "<-", "=", "<<-", "+", "-", "*", "/", "^", "==", "!=", "<", "<=", ">=", ">", "&", "|", "!", "&&", "||", "~"
};

static const set<string> BASE_INFIX_FUNS_NO_SPACE = {
    ":", "::", ":::", "$", "@"
};

// "keywords" that need to be escaped
// cf. https://stat.ethz.ch/R-manual/R-devel/library/base/html/Reserved.html
static const set<string> KEYWORDS = {
    "if", "else", "repeat", "while", "function", "for", "in", "next", "break",
    "TRUE", "FALSE", "NULL", "Inf", "NaN", "NA", "NA_integer_", "NA_real_",
    "NA_complex_", "NA_character_", "..."
};

// A syntactically valid name consists of letters, numbers and the dot or underline
// characters and starts with a letter or the dot not followed by a number
// cf. https://stat.ethz.ch/R-manual/R-devel/library/base/html/make.names.html
static const regex VALID_NAME = regex("^([a-zA-Z][a-zA-Z0-9._]*|[.]([a-zA-Z._][a-zA-Z0-9._]*)?)$");

static const map<SEXP, string> SPEC_ATTRIBUTES_NAMES = {
    {R_DimSymbol, ".Dim"},
    {R_DimNamesSymbol, ".Dimnames"},
    {R_TspSymbol, ".Tsp"},
    {R_NamesSymbol, ".Names"},
    {R_LevelsSymbol, ".Label"}
};

static SEXP GENTHAT_EXTRACTED_CLOSURE_SYM = Rf_install("genthat_extracted_closure");

class Serializer {
private:
    static bool is_infix_fun_no_space(string const &fun) {
        return BASE_INFIX_FUNS_NO_SPACE.find(fun) != BASE_INFIX_FUNS_NO_SPACE.end();
    }

    static bool is_infix_fun(string const &fun) {
        if (BASE_INFIX_FUNS.find(fun) != BASE_INFIX_FUNS.end()) {
            return true;
        } else if (BASE_INFIX_FUNS_NO_SPACE.find(fun) != BASE_INFIX_FUNS_NO_SPACE.end()) {
            return true;
        } else if (fun[0] == '%' && fun[fun.size() - 1] == '%') {
            return true;
        } else {
            return false;
        }
    }

    static string escape_name(string const &name) {
        if (name.empty()) {
            return name;
        } else if (KEYWORDS.find(name) != KEYWORDS.end() || !regex_match(name, VALID_NAME)) {
            return "`" + name + "`";
        } else {
            return name;
        }
    }

    static string attribute_name(SEXP const s) {
        auto e = SPEC_ATTRIBUTES_NAMES.find(s);
        if (e != SPEC_ATTRIBUTES_NAMES.end()) {
            return e->second;
        } else {
            string tag = CHAR(PRINTNAME(s));
            return escape_name(tag);
        }
    }

    string wrap_in_attributes(SEXP const s, string const &s_str) {
        RObject protected_s(s);

        string elems = "";
        for (SEXP a = ATTRIB(s); !Rf_isNull(a); a = CDR(a)) {
            SEXP tag = TAG(a);

            if (tag == Rf_install("srcref")) {
                continue;
            } else if (tag == R_NamesSymbol) {
                continue;
            } else {
                string name = attribute_name(tag);
                elems += name + "=" + serialize(CAR(a), true);
                elems += !Rf_isNull(CDR(a)) ? ", " : "";
            }
        }

        return !elems.empty() ?  "structure(" + s_str + ", " + elems + ")" : s_str;
    }

    string format_argument(SEXP const arg) {
        SEXP arg_name = TAG(arg);
        SEXP arg_value = CAR(arg);

        string name;

        switch (TYPEOF(arg_name)) {
        case NILSXP:
            name = "";
            break;
        case SYMSXP:
            name = escape_name(serialize(arg_name, false));
            break;
        default:
            throw serialization_error("Unexpected SEXPTYPE in function arguments: " + to_string(TYPEOF(arg_name)));
        }

        string value = serialize(arg_value, false);
        string res;

        if (!name.empty() && !value.empty()) {
            res = name + "=" + value;
        } else if (name.empty()) {
            res = value;
        } else if (value.empty()) {
            res = name;
        } else {
            res = "";
        }

        return res;
    }

    string format_arguments(SEXP const args, string const &sep=", ") {
        string res = "";

        for (SEXP arg = args; !Rf_isNull(arg); arg = CDR(arg)) {
            res += format_argument(arg);
            res += Rf_isNull(CDR(arg)) ? "" : sep;
        }

        return res;
    }

    string get_element_name(SEXP const names, int i) {
        string name;

        if (!Rf_isNull(names)) {
            name = CHAR(STRING_ELT(names, i));
        }

        return name;
    }

    SEXP extract_closure(SEXP fun) {
        // TODO: make sure it is a function
        Environment genthat("package:genthat");
        Function extract_closure_r = genthat["extract_closure"];

        SEXP extracted = extract_closure_r(Rcpp::_["fun"] = fun);

        // remove the attribute indicating the extracted closure
        // so not to pollute the output
        Rf_setAttrib(extracted, GENTHAT_EXTRACTED_CLOSURE_SYM, R_NilValue);

        return extracted;
    }

    string concatenate(StringVector v, string sep) {
        string res;

        for (int i = 0; i < v.size(); i++) {
            res += v(i);
            res += i + 1 < v.size() ? sep : "";
        }

        return res;
    }

    // contains the list of visited environments so far
    // it is used for the ENVSXP serialization
    set<SEXP> visited_environments;

public:
    string serialize(SEXP s, bool quote) {
        switch (TYPEOF(s)) {
        case NILSXP:
            return "NULL";
        case VECSXP: { /* lists */
            RObject protected_s(s);

            SEXP names = Rf_getAttrib(s, R_NamesSymbol);
            int size = XLENGTH(s);
            string args;

            for (int i = 0 ; i < size ; i++) {
                string value = serialize(VECTOR_ELT(s, i), true);
                string name = escape_name(get_element_name(names, i));

                args += name.empty() ? name : name + "=";
                args += value;
                args += i + 1 < size ? ", " : "";
            }

            return wrap_in_attributes(s, "list(" + args + ")");
        }
            // all the primitive vectors should be serialized by SEXP deparse1(SEXP call, Rboolean abbrev, int opts)
        case LGLSXP:
        case INTSXP:
        case REALSXP:
        case CPLXSXP:
        case STRSXP: {
            StringVector deparsed = Rf_deparse1(s, FALSE, KEEPINTEGER | SHOWATTRIBUTES | KEEPNA | DIGITS16);
            string res = concatenate(deparsed, "\n");
            return res;
        }
        case SYMSXP: {
            RObject protected_s(s);
            string symbol = string(CHAR(PRINTNAME(s)));

            if (symbol.empty()) {
                return quote ? "quote(expr=)" : "";
            } else {
                return quote ? "quote(" + symbol  + ")" : symbol;
            }
        }
        case ENVSXP: {
            RObject protected_s(s);

            if (visited_environments.find(s) != visited_environments.end()) {
                throw cycle_error();
            }

            visited_environments.insert(s);

            SEXP parent = ENCLOS(s);
            SEXP names = R_lsInternal3(s, TRUE, FALSE);
            int n = XLENGTH(names);

            string elems;
            for (int i = 0; i < n; i++) {
                const char *key = CHAR(STRING_ELT(names, i));
                SEXP value = Rf_findVarInFrame(s, Rf_install(key));

                elems += escape_name(key) + "=" + serialize(value, true);
                elems += i + 1 < n ? ", " : "";
            }

            string parent_env_arg;
            if (!Rf_isNull(parent) && visited_environments.find(parent) == visited_environments.end()) {
                string parent_env;

                if (parent == R_EmptyEnv) {
                    parent_env = "emptyenv()";
                } else if (parent == R_GlobalEnv) {
                    parent_env = "globalenv()";
                } else if (parent == R_BaseEnv || parent == R_BaseNamespace) {
                    parent_env = "baseenv()";
                } else if (R_IsPackageEnv(parent) == TRUE) {
                    // cf. builtin.c:432 do_envirName
                    string parent_name = CHAR(STRING_ELT(R_PackageEnvName(parent), 0));

                    parent_env = "as.environment(\"" + parent_name + "\")";
                } else if (R_IsNamespaceEnv(parent) == TRUE) {
                    // cf. builtin.c:434 do_envirName
                    string parent_name = CHAR(STRING_ELT(R_NamespaceEnvSpec(parent), 0));

                    parent_env = "getNamespace(\"" + parent_name + "\")";
                } else {
                    parent_env = serialize(parent, false);
                }

                parent_env_arg = ", parent=" + parent_env;
            }

            visited_environments.erase(s);

            return "list2env(list(" + elems + ")" + parent_env_arg + ")";
        }
        // TODO: do we need this one?
        case LISTSXP: {/* pairlists */

            RObject protected_s(s);
            stringstream outStr;
            outStr << "\"alist(";
            SEXP names = Rf_getAttrib(s, R_NamesSymbol);
            int i = 0;

            for (SEXP con = s; con != R_NilValue; con = CDR(con))
            {
                if (i != 0) outStr << ", ";

                outStr << escape_name(CHAR(STRING_ELT(names, i++))) << " = ";
                auto val = serialize(CAR(con), false);
                if (val != "")
                    outStr << val;
            }
            outStr << ")\"";
            return outStr.str();
        }
        case LANGSXP: {
            RObject protected_s(s);
            string fun = serialize(CAR(s), false);
            string res;
            s = CDR(s);

            if (is_infix_fun(fun)) {
                SEXP lhs = CAR(s);
                SEXP rhs = CADR(s);

                string space = is_infix_fun_no_space(fun) ? "" : " ";

                res =
                    serialize(lhs, false) +
                    space + fun + space +
                    serialize(rhs, false);

            } else if (fun[0] == '[') {
                string collection = serialize(CAR(s), false);
                string subset = format_arguments(CDR(s));
                string close;

                if (fun == "[") {
                    close = "]";
                } else if (fun == "[[") {
                    close = "]]";
                } else {
                    throw serialization_error("Unknown sub-setting operator: " + fun);
                }

                res = collection + fun + subset + close;
            } else if (fun == "function") {
                string args = format_arguments(CAR(s));
                string body = serialize(CADR(s), false);

                res = fun + "(" + args + ") " + body;
            } else if (fun == "{") {
                string args = format_arguments(s, "\n\t");
                res = "{\n\t" + args + "\n}";
            } else if (fun == "(") {
                string args = format_arguments(s);
                res = "(" + args + ")";
            } else {
                string args = format_arguments(s);
                res = fun + "(" + args + ")";
            }

            return res;
        }
            // the following is annoying, but the sexptype2char from memory.c is not public
        case SPECIALSXP:
            throw sexp_not_supported_error("SPECIALSXP");
        case BUILTINSXP:
            throw sexp_not_supported_error("BUILTINSXP");
        case EXTPTRSXP:
            throw sexp_not_supported_error("EXTPTRSXP");
        case BCODESXP:
            throw sexp_not_supported_error("BCODESXP");
        case WEAKREFSXP:
            throw sexp_not_supported_error("WEAKREFSXP");
        case CLOSXP: {
            SEXP extracted = extract_closure(s);
            SEXP env = CLOENV(extracted);
            string env_code = environment_name_as_code(env);

            // if this is empty, the environment is not a empty / base / package / namespace
            // and therefore we need to serialize it
            if (env_code.empty()) {
                env_code = serialize(env, false);
            }

            string fun_code = concatenate(Rf_deparse1(extracted, FALSE, KEEPINTEGER | SHOWATTRIBUTES | KEEPNA | DIGITS16), "\n");
            string res = "genthat::with_env(" + fun_code + ", env=" + env_code + ")";

            return res;
        }
        case DOTSXP:
            throw sexp_not_supported_error("DOTSXP");
        case CHARSXP:
            throw sexp_not_supported_error("CHARSXP");
        case EXPRSXP:
            throw sexp_not_supported_error("EXPRSXP");
        case RAWSXP:
            throw sexp_not_supported_error("RAWSXP");
        case PROMSXP: {
            // s = Rf_eval(s, R_BaseEnv);
            // return serialize(s, quote);
            throw sexp_not_supported_error("PROMSXP");
        }
        case S4SXP:
            throw sexp_not_supported_error("S4SXP");
        default:
            throw sexp_not_supported_error("unknown");
        }
    }
};

// [[Rcpp::export]]
std::string serialize_value(SEXP s) {
    Serializer serializer;

    return serializer.serialize(s, false);
}
