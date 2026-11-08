/**
 * @file      web_tool_script.cpp
 * @brief     A small sh-like scripting engine shared by the web Scripts app and the native
 *            on-device Scripts app (ui_script.cpp). One interpreter, two thin front-ends — the
 *            same relationship the Terminal (web_tool_term.cpp) has with its two UIs.
 *
 * script_run() is a pure function: it takes a script source and a working directory and returns
 * the output text. It is NOT bash — it is a deliberately small, Linux-flavoured subset that turns
 * the fixed command set into a programmable platform:
 *
 *   - comments           # to end of line (at a word boundary)
 *   - separators         newline and ';'
 *   - variables          NAME=value ; expansion $NAME, ${NAME}, $? (last exit status)
 *   - quoting            'single' (literal)  "double" (expands $…)
 *   - arithmetic         $(( expr ))  with + - * / % and ( ), integer, bare names read vars
 *   - builtins           echo* set sleep test / [ … ] true false : exit
 *   - control flow       if LIST; then … elif … else … fi
 *                        for NAME in WORDS; do … done
 *                        while LIST; do … done
 *   - operators          && and || on exit status
 *
 * Everything that is not a builtin is handed to the Terminal shell (term_exec in web_tool_term.cpp),
 * so `ls`, `cat`, `status`, `cd`, `logs`, … behave exactly as they do in the Terminal and stay
 * sandboxed to /sd and /flash. (*echo is one of those, routed to term_exec after expansion, so the
 * two agree.) External commands report the shell's real exit status in $? (0 success, 1 a failed
 * command, 127 command not found), so `if cmd`, `while cmd`, `&&` and `||` branch on whether the
 * command actually succeeded — not only on test/[ … ].
 *
 * Scripts live in /sd/scripts as .scr files. The run is synchronous and bounded (step, wall-clock and
 * output caps, and a per-sleep cap) so a runaway or infinite loop cannot hang the device.
 *
 * The JavaScript mock (tools/web_mock_device.mjs) mirrors this file line-for-line so both test
 * planes agree, exactly as it does for the Terminal.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include "web_internal.h"
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <string>
#include <vector>

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <time.h>
#endif

/* ---------------------------------------------------------------- limits (bound a run) */

#define SCRIPT_SRC_MAX   (16 * 1024)    /* largest source accepted / stored */
#define SCRIPT_OUT_MAX   (32 * 1024)    /* cap on total output text */
#define SCRIPT_STEP_MAX  20000          /* commands executed before we abort */
#define SCRIPT_MS_MAX    8000           /* wall-clock budget for a whole run */
#define SCRIPT_SLEEP_MAX 5000           /* cap on one sleep, ms */
#define SCRIPT_DEPTH_MAX 24             /* nesting of if/for/while */
#define SCRIPT_DIR       "/sd/scripts"

/* ---------------------------------------------------------------- AST */

struct Seg { int type; std::string text; };            /* 0 literal, 1 $var (text=name), 2 $((arith)) */
typedef std::vector<Seg> Word;

struct Assign { std::string name; Word value; };

enum NType { N_CMD, N_ANDOR, N_IF, N_FOR, N_WHILE };

struct Node {
    NType t = N_CMD;
    /* N_CMD */
    std::vector<Assign> assigns;
    std::vector<Word> argv;
    /* N_ANDOR */
    int op = 0;                                        /* 0 = &&, 1 = || */
    std::vector<Node> left, right;                     /* each size 1 */
    /* N_IF / N_WHILE */
    std::vector<Node> cond;                            /* size 1 (a simple node) */
    std::vector<Node> body;
    std::vector<std::vector<Node>> elifConds, elifBodies;
    std::vector<Node> elseBody;
    bool hasElse = false;
    /* N_FOR */
    std::string var;
    std::vector<Word> items;
};

/* ---------------------------------------------------------------- lexer */

static bool is_name_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static bool is_name_char(char c) { return is_name_start(c) || (c >= '0' && c <= '9'); }

/* Lex source into statements (each a list of words, each word a list of segments). On a syntax
 * error returns false with err set. */
static bool lex(const std::string &src, std::vector<std::vector<Word>> &stmts, std::string &err)
{
    std::vector<Word> stmt;
    Word word;
    std::string lit;
    bool word_active = false;
    int quote = 0;                                     /* 0 none, 1 single, 2 double */

    auto flush_lit = [&] { if (!lit.empty()) { word.push_back({0, lit}); lit.clear(); } };
    auto flush_word = [&] { flush_lit(); if (word_active) { stmt.push_back(word); word.clear(); word_active = false; } };
    auto end_stmt = [&] { flush_word(); if (!stmt.empty()) { stmts.push_back(stmt); stmt.clear(); } };

    size_t i = 0, n = src.size();
    while (i < n) {
        char c = src[i];
        if (quote == 0) {
            if (c == '#' && !word_active) {            /* comment to end of line */
                while (i < n && src[i] != '\n') i++;
                continue;
            }
            if (c == '\n' || c == ';') { end_stmt(); i++; continue; }
            if (c == ' ' || c == '\t' || c == '\r') { flush_word(); i++; continue; }
            if (c == '\'') { word_active = true; quote = 1; i++; continue; }
            if (c == '"')  { word_active = true; quote = 2; i++; continue; }
            if (c == '$')  { word_active = true; /* fall through to $ handling */ }
            else { lit += c; word_active = true; i++; continue; }
        } else if (quote == 1) {                       /* single quote: literal */
            if (c == '\'') { quote = 0; i++; continue; }
            lit += c; i++; continue;
        } else {                                       /* double quote: expands $ */
            if (c == '"') { quote = 0; i++; continue; }
            if (c != '$') { lit += c; i++; continue; }
        }
        /* c == '$' (unquoted or inside double quote) */
        if (i + 1 < n && src[i + 1] == '{') {
            size_t j = src.find('}', i + 2);
            if (j == std::string::npos) { err = "bad substitution"; return false; }
            flush_lit();
            word.push_back({1, src.substr(i + 2, j - (i + 2))});
            i = j + 1;
        } else if (i + 2 < n && src[i + 1] == '(' && src[i + 2] == '(') {
            size_t j = src.find("))", i + 3);
            if (j == std::string::npos) { err = "bad arithmetic"; return false; }
            flush_lit();
            word.push_back({2, src.substr(i + 3, j - (i + 3))});
            i = j + 2;
        } else if (i + 1 < n && src[i + 1] == '?') {
            flush_lit();
            word.push_back({1, "?"});
            i += 2;
        } else if (i + 1 < n && is_name_start(src[i + 1])) {
            size_t j = i + 1;
            while (j < n && is_name_char(src[j])) j++;
            flush_lit();
            word.push_back({1, src.substr(i + 1, j - (i + 1))});
            i = j;
        } else {
            lit += '$';                                /* a bare $ */
            i++;
        }
    }
    if (quote != 0) { err = "unterminated quote"; return false; }
    end_stmt();
    return true;
}

/* A word is a bare keyword if it is a single literal segment equal to kw. */
static bool word_is(const Word &w, const char *kw) { return w.size() == 1 && w[0].type == 0 && w[0].text == kw; }
static std::string stmt_kw(const std::vector<Word> &s)
{
    if (s.empty() || s[0].size() != 1 || s[0][0].type != 0) return "";
    const std::string &t = s[0][0].text;
    if (t == "if" || t == "then" || t == "elif" || t == "else" || t == "fi" ||
        t == "for" || t == "in" || t == "do" || t == "done" || t == "while") return t;
    return "";
}

/* then/do/else may carry the first body statement on the same line ("then echo hi"); split those
 * off so each is its own statement before parsing. */
static void split_trailing_keywords(std::vector<std::vector<Word>> &stmts)
{
    for (size_t i = 0; i < stmts.size(); i++) {
        std::string k = stmt_kw(stmts[i]);
        if ((k == "then" || k == "do" || k == "else") && stmts[i].size() > 1) {
            std::vector<Word> rest(stmts[i].begin() + 1, stmts[i].end());
            stmts[i].resize(1);
            stmts.insert(stmts.begin() + i + 1, rest);
        }
    }
}

/* ---------------------------------------------------------------- parser */

struct Parser {
    const std::vector<std::vector<Word>> &stmts;
    size_t pos = 0;
    std::string err;
    explicit Parser(const std::vector<std::vector<Word>> &s) : stmts(s) {}

    bool fail(const std::string &m) { if (err.empty()) err = m; return false; }
    std::string kw() { return pos < stmts.size() ? stmt_kw(stmts[pos]) : ""; }

    /* Build a simple node (command, or && / || chain) from a list of words. */
    bool simple_from_words(const std::vector<Word> &words, Node &out)
    {
        std::vector<std::vector<Word>> chunks(1);
        std::vector<int> ops;
        for (const Word &w : words) {
            if (word_is(w, "&&") || word_is(w, "||")) {
                if (chunks.back().empty()) return fail("syntax error near '" + w[0].text + "'");
                ops.push_back(word_is(w, "&&") ? 0 : 1);
                chunks.push_back({});
            } else {
                chunks.back().push_back(w);
            }
        }
        if (chunks.back().empty()) return fail("syntax error: empty command");

        auto make_cmd = [&](const std::vector<Word> &ws, Node &cmd) -> bool {
            cmd.t = N_CMD;
            size_t k = 0;
            for (; k < ws.size(); k++) {               /* leading NAME=value assignments */
                const Word &w = ws[k];
                if (w.empty() || w[0].type != 0) break;
                const std::string &first = w[0].text;
                size_t eq = first.find('=');
                if (eq == std::string::npos || eq == 0 || !is_name_start(first[0])) break;
                bool ok = true;
                for (size_t c = 1; c < eq; c++) if (!is_name_char(first[c])) { ok = false; break; }
                if (!ok) break;
                Assign a;
                a.name = first.substr(0, eq);
                a.value.push_back({0, first.substr(eq + 1)});
                for (size_t s = 1; s < w.size(); s++) a.value.push_back(w[s]);
                cmd.assigns.push_back(a);
            }
            for (; k < ws.size(); k++) cmd.argv.push_back(ws[k]);
            return true;
        };

        Node node;
        if (!make_cmd(chunks[0], node)) return false;
        for (size_t c = 0; c < ops.size(); c++) {
            Node rhs;
            if (!make_cmd(chunks[c + 1], rhs)) return false;
            Node join;
            join.t = N_ANDOR;
            join.op = ops[c];
            join.left.push_back(node);
            join.right.push_back(rhs);
            node = join;
        }
        out = node;
        return true;
    }

    bool parse_block(const std::vector<std::string> &stop, int depth, std::vector<Node> &out)
    {
        if (depth > SCRIPT_DEPTH_MAX) return fail("too deeply nested");
        while (pos < stmts.size()) {
            std::string k = kw();
            for (const std::string &s : stop) if (k == s) return true;
            if (k == "then" || k == "elif" || k == "else" || k == "fi" || k == "done" || k == "in")
                return fail("unexpected '" + k + "'");
            Node n;
            if (k == "if") { if (!parse_if(depth + 1, n)) return false; }
            else if (k == "for") { if (!parse_for(depth + 1, n)) return false; }
            else if (k == "while") { if (!parse_while(depth + 1, n)) return false; }
            else { if (!simple_from_words(stmts[pos], n)) return false; pos++; }
            out.push_back(n);
        }
        return true;
    }

    /* words after the keyword of the current statement */
    std::vector<Word> tail() { return std::vector<Word>(stmts[pos].begin() + 1, stmts[pos].end()); }

    bool parse_if(int depth, Node &out)
    {
        out.t = N_IF;
        std::vector<Word> c = tail();
        if (c.empty()) return fail("if: missing condition");
        pos++;
        Node cn; if (!simple_from_words(c, cn)) return false;
        out.cond.push_back(cn);
        if (kw() != "then") return fail("if: missing 'then'");
        pos++;
        if (!parse_block({"elif", "else", "fi"}, depth, out.body)) return false;
        while (true) {
            std::string k = kw();
            if (k == "elif") {
                std::vector<Word> ec = tail();
                if (ec.empty()) return fail("elif: missing condition");
                pos++;
                Node ecn; if (!simple_from_words(ec, ecn)) return false;
                if (kw() != "then") return fail("elif: missing 'then'");
                pos++;
                std::vector<Node> eb;
                if (!parse_block({"elif", "else", "fi"}, depth, eb)) return false;
                out.elifConds.push_back({ecn});
                out.elifBodies.push_back(eb);
            } else if (k == "else") {
                pos++;
                if (!parse_block({"fi"}, depth, out.elseBody)) return false;
                out.hasElse = true;
            } else if (k == "fi") {
                pos++;
                return true;
            } else {
                return fail("if: missing 'fi'");
            }
        }
    }

    bool parse_for(int depth, Node &out)
    {
        out.t = N_FOR;
        const std::vector<Word> &w = stmts[pos];
        if (w.size() < 2 || w[1].size() != 1 || w[1][0].type != 0 || !is_name_start(w[1][0].text[0]))
            return fail("for: missing variable");
        out.var = w[1][0].text;
        for (char ch : out.var) if (!is_name_char(ch)) return fail("for: bad variable");
        if (w.size() < 3 || !word_is(w[2], "in")) return fail("for: missing 'in'");
        for (size_t k = 3; k < w.size(); k++) out.items.push_back(w[k]);
        pos++;
        if (kw() != "do") return fail("for: missing 'do'");
        pos++;
        if (!parse_block({"done"}, depth, out.body)) return false;
        if (kw() != "done") return fail("for: missing 'done'");
        pos++;
        return true;
    }

    bool parse_while(int depth, Node &out)
    {
        out.t = N_WHILE;
        std::vector<Word> c = tail();
        if (c.empty()) return fail("while: missing condition");
        pos++;
        Node cn; if (!simple_from_words(c, cn)) return false;
        out.cond.push_back(cn);
        if (kw() != "do") return fail("while: missing 'do'");
        pos++;
        if (!parse_block({"done"}, depth, out.body)) return false;
        if (kw() != "done") return fail("while: missing 'done'");
        pos++;
        return true;
    }
};

/* ---------------------------------------------------------------- arithmetic $(( )) */

struct Arith {
    const std::string &s;
    size_t i = 0;
    const std::vector<std::pair<std::string, std::string>> &vars;
    Arith(const std::string &e, const std::vector<std::pair<std::string, std::string>> &v) : s(e), vars(v) {}

    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++; }
    long long var_val(const std::string &name)
    {
        for (const auto &kv : vars) if (kv.first == name) return atoll(kv.second.c_str());
        return 0;
    }
    long long factor()
    {
        ws();
        if (i < s.size() && s[i] == '(') { i++; long long v = expr(); ws(); if (i < s.size() && s[i] == ')') i++; return v; }
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) { char op = s[i++]; long long v = factor(); return op == '-' ? -v : v; }
        if (i < s.size() && (s[i] >= '0' && s[i] <= '9')) {
            long long v = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
            return v;
        }
        if (i < s.size() && is_name_start(s[i])) {
            size_t a = i;
            while (i < s.size() && is_name_char(s[i])) i++;
            return var_val(s.substr(a, i - a));
        }
        return 0;
    }
    long long term()
    {
        long long v = factor();
        while (true) {
            ws();
            if (i < s.size() && (s[i] == '*' || s[i] == '/' || s[i] == '%')) {
                char op = s[i++];
                long long r = factor();
                if (op == '*') v *= r;
                else if (r == 0) v = 0;                /* guard divide/modulo by zero */
                else if (op == '/') v /= r;
                else v %= r;
            } else break;
        }
        return v;
    }
    long long expr()
    {
        long long v = term();
        while (true) {
            ws();
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) { char op = s[i++]; long long r = term(); v = op == '+' ? v + r : v - r; }
            else break;
        }
        return v;
    }
};

/* ---------------------------------------------------------------- interpreter */

struct Ctx {
    std::string cwd;
    std::string out;
    std::vector<std::pair<std::string, std::string>> vars;    /* insertion order, small */
    int status = 0;
    long steps = 0;
    uint32_t start = 0;
    bool exited = false;
    bool aborted = false;
};

static std::string *var_slot(Ctx &c, const std::string &name)
{
    for (auto &kv : c.vars) if (kv.first == name) return &kv.second;
    return NULL;
}
static void var_set(Ctx &c, const std::string &name, const std::string &val)
{
    if (std::string *s = var_slot(c, name)) { *s = val; return; }
    c.vars.push_back({name, val});
}

static void out_add(Ctx &c, const std::string &s)
{
    if (c.out.size() < SCRIPT_OUT_MAX) c.out += s;
}
static void out_line(Ctx &c, const std::string &s) { out_add(c, s + "\n"); }

static std::string expand_word(Ctx &c, const Word &w)
{
    std::string r;
    for (const Seg &seg : w) {
        if (seg.type == 0) r += seg.text;
        else if (seg.type == 1) {
            if (seg.text == "?") r += tools_int(c.status);
            else if (std::string *s = var_slot(c, seg.text)) r += *s;
        } else {                                       /* arithmetic */
            Arith a(seg.text, c.vars);
            r += tools_int(a.expr());
        }
    }
    return r;
}

static bool check_caps(Ctx &c)
{
    if (c.aborted) return true;
    const char *why = NULL;
    if (++c.steps > SCRIPT_STEP_MAX) why = "step limit";
    else if (tools_now_ms() - c.start > SCRIPT_MS_MAX) why = "time limit";
    else if (c.out.size() >= SCRIPT_OUT_MAX) why = "output limit";
    if (why) { c.aborted = true; c.out += std::string("\nscript: aborted (") + why + ")\n"; return true; }
    return false;
}

static void sleep_ms(uint32_t ms)
{
#ifdef ARDUINO
    delay(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* file test for [ -e / -f / -d PATH ] */
static int path_stat(Ctx &c, const std::string &arg, struct stat &st)
{
    std::string api = arg.empty() ? c.cwd : (arg[0] == '/' ? arg : c.cwd + "/" + arg);
    std::string host;
    if (web_fs_resolve(api.c_str(), host)) return -1;
    return stat(host.c_str(), &st);
}

static int eval_test(Ctx &c, const std::vector<std::string> &a)
{
    size_t n = a.size();
    if (n == 0) return 1;
    if (n == 1) return a[0].empty() ? 1 : 0;
    if (n == 2) {
        const std::string &op = a[0], &x = a[1];
        if (op == "-z") return x.empty() ? 0 : 1;
        if (op == "-n") return x.empty() ? 1 : 0;
        if (op == "-e" || op == "-f" || op == "-d") {
            struct stat st;
            if (path_stat(c, x, st) != 0) return 1;
            if (op == "-d") return S_ISDIR(st.st_mode) ? 0 : 1;
            if (op == "-f") return S_ISREG(st.st_mode) ? 0 : 1;
            return 0;
        }
        return 2;
    }
    if (n == 3) {
        const std::string &x = a[0], &op = a[1], &y = a[2];
        if (op == "=" || op == "==") return x == y ? 0 : 1;
        if (op == "!=") return x != y ? 0 : 1;
        long long ix = atoll(x.c_str()), iy = atoll(y.c_str());
        if (op == "-eq") return ix == iy ? 0 : 1;
        if (op == "-ne") return ix != iy ? 0 : 1;
        if (op == "-lt") return ix < iy ? 0 : 1;
        if (op == "-le") return ix <= iy ? 0 : 1;
        if (op == "-gt") return ix > iy ? 0 : 1;
        if (op == "-ge") return ix >= iy ? 0 : 1;
        return 2;
    }
    return 2;
}

static void run_nodes(Ctx &c, const std::vector<Node> &nodes);

/* Run one command (argv already expanded). Updates c.status. */
static void run_argv(Ctx &c, const std::vector<std::string> &argv)
{
    if (argv.empty()) { c.status = 0; return; }
    const std::string &cmd = argv[0];

    if (cmd == "help" || cmd == "?") {
        static const char *const H[] = {
            "scripting engine — a small sh-like language.",
            "variables   NAME=value ; expand $NAME ${NAME} $? ; $(( expr )) integer maths",
            "quoting     'literal'   \"expands $vars\"   # comment to end of line",
            "echo TEXT   print (expanded)        set    list variables",
            "test / [ … ]  -z -n -e -f -d, = != -eq -ne -lt -le -gt -ge",
            "sleep N     wait N seconds          true / false / :   exit status 0 / 1 / 0",
            "exit [N]    stop the script",
            "if LIST; then … elif … else … fi",
            "for NAME in WORDS; do … done        while LIST; do … done",
            "LIST && LIST   LIST || LIST",
            "any other command (ls, cat, status, cd, logs, …) runs in the Terminal shell.",
            NULL,
        };
        for (const char *const *h = H; *h; h++) out_line(c, *h);
        c.status = 0;
        return;
    }
    if (cmd == ":") { c.status = 0; return; }
    if (cmd == "true") { c.status = 0; return; }
    if (cmd == "false") { c.status = 1; return; }
    if (cmd == "exit") { c.exited = true; if (argv.size() > 1) c.status = atoi(argv[1].c_str()); return; }
    if (cmd == "sleep") {
        if (argv.size() < 2) { out_line(c, "sleep: missing seconds"); c.status = 1; return; }
        double sec = atof(argv[1].c_str());
        if (sec < 0) sec = 0;
        long ms = (long)(sec * 1000.0);
        if (ms > SCRIPT_SLEEP_MAX) ms = SCRIPT_SLEEP_MAX;
        sleep_ms((uint32_t)ms);
        c.status = 0;
        return;
    }
    if (cmd == "set" && argv.size() == 1) {
        std::vector<std::string> lines;
        for (const auto &kv : c.vars) lines.push_back(kv.first + "=" + kv.second);
        std::sort(lines.begin(), lines.end());
        for (const std::string &l : lines) out_line(c, l);
        c.status = 0;
        return;
    }
    if (cmd == "test" || cmd == "[") {
        std::vector<std::string> a(argv.begin() + 1, argv.end());
        if (cmd == "[") {
            if (a.empty() || a.back() != "]") { out_line(c, "[: missing ']'"); c.status = 2; return; }
            a.pop_back();
        }
        c.status = eval_test(c, a);
        return;
    }

    /* Everything else is a Terminal command (ls, cat, cd, status, echo, …). Reconstruct the line
     * from the expanded argv and hand it to the shared shell; it stays sandboxed there. Its real exit
     * status (0 success, 1 a failed command, 127 command not found) becomes $?, so `if cmd`, `while
     * cmd`, `&&` and `||` branch on whether the command actually succeeded. */
    std::string line;
    for (size_t i = 0; i < argv.size(); i++) { if (i) line += ' '; line += argv[i]; }
    std::string out;
    c.status = term_exec(c.cwd, line, out);
    out_add(c, out);
}

static void run_simple(Ctx &c, const Node &n)
{
    if (c.exited || c.aborted) return;
    if (n.t == N_ANDOR) {
        run_simple(c, n.left[0]);
        if (c.exited || c.aborted) return;
        if ((n.op == 0 && c.status == 0) || (n.op == 1 && c.status != 0)) run_simple(c, n.right[0]);
        return;
    }
    /* N_CMD */
    for (const Assign &a : n.assigns) var_set(c, a.name, expand_word(c, a.value));
    if (n.argv.empty()) { c.status = 0; return; }      /* pure assignment(s) */
    if (check_caps(c)) return;
    std::vector<std::string> argv;
    argv.reserve(n.argv.size());
    for (const Word &w : n.argv) argv.push_back(expand_word(c, w));
    run_argv(c, argv);
}

static void run_nodes(Ctx &c, const std::vector<Node> &nodes)
{
    for (const Node &n : nodes) {
        if (c.exited || c.aborted) return;
        switch (n.t) {
        case N_CMD:
        case N_ANDOR:
            run_simple(c, n);
            break;
        case N_IF: {
            run_simple(c, n.cond[0]);
            if (c.exited || c.aborted) return;
            if (c.status == 0) { run_nodes(c, n.body); break; }
            bool done = false;
            for (size_t k = 0; k < n.elifConds.size(); k++) {
                run_simple(c, n.elifConds[k][0]);
                if (c.exited || c.aborted) return;
                if (c.status == 0) { run_nodes(c, n.elifBodies[k]); done = true; break; }
            }
            if (!done && n.hasElse) run_nodes(c, n.elseBody);
            break;
        }
        case N_FOR:
            for (const Word &item : n.items) {
                if (check_caps(c)) return;
                var_set(c, n.var, expand_word(c, item));
                run_nodes(c, n.body);
                if (c.exited || c.aborted) return;
            }
            break;
        case N_WHILE:
            while (true) {
                if (check_caps(c)) return;
                run_simple(c, n.cond[0]);
                if (c.exited || c.aborted) return;
                if (c.status != 0) break;
                run_nodes(c, n.body);
                if (c.exited || c.aborted) return;
            }
            break;
        }
    }
}

/* ---------------------------------------------------------------- public entry */

void script_run(std::string &cwd, const std::string &src, std::string &out)
{
    if (cwd.empty()) cwd = "/sd";
    std::vector<std::vector<Word>> stmts;
    std::string err;
    if (!lex(src, stmts, err)) { out += "script: " + err + "\n"; return; }
    split_trailing_keywords(stmts);
    Parser p(stmts);
    std::vector<Node> prog;
    if (!p.parse_block({}, 0, prog) || !p.err.empty()) { out += "script: " + p.err + "\n"; return; }

    Ctx c;
    c.cwd = cwd;
    c.start = tools_now_ms();
    run_nodes(c, prog);
    cwd = c.cwd;
    out += c.out;
}

/* ---------------------------------------------------------------- script library on /sd */

static bool script_name_ok(const std::string &name)
{
    if (name.size() < 1 || name.size() > 48) return false;
    for (char c : name) if (!(is_name_char(c) || c == '.' || c == '-')) return false;
    if (name.find("..") != std::string::npos) return false;
    return name.size() > 4 && name.compare(name.size() - 4, 4, ".scr") == 0;
}

/* Names of the .scr files in /sd/scripts, sorted. Used by the native Scripts app (ui_script.cpp). */
void script_list_names(std::vector<std::string> &out)
{
    out.clear();
    std::string host;
    if (web_fs_resolve(SCRIPT_DIR, host) != 0) return;
    DIR *d = opendir(host.c_str());
    if (!d) return;
    while (struct dirent *ent = readdir(d)) {
        std::string nm = ent->d_name;
        if (!script_name_ok(nm)) continue;
        struct stat st;
        if (stat((host + "/" + nm).c_str(), &st) == 0 && S_ISREG(st.st_mode)) out.push_back(nm);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
}

/* Read a named script's source (sandboxed to /sd/scripts). false if the name is bad or unreadable. */
bool script_read_file(const std::string &name, std::string &src)
{
    src.clear();
    if (!script_name_ok(name)) return false;
    std::string host;
    if (web_fs_resolve(std::string(SCRIPT_DIR "/" + name).c_str(), host) != 0) return false;
    FILE *f = fopen(host.c_str(), "rb");
    if (!f) return false;
    char chunk[512];
    size_t rd;
    while ((rd = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        src.append(chunk, rd);
        if (src.size() > SCRIPT_SRC_MAX) { src.resize(SCRIPT_SRC_MAX); break; }
    }
    fclose(f);
    return true;
}

static void scripts_list(std::string &out)
{
    out = "{\"dir\":\"" SCRIPT_DIR "\",\"scripts\":[";
    std::string host;
    if (web_fs_resolve(SCRIPT_DIR, host) == 0) {
        DIR *d = opendir(host.c_str());
        if (d) {
            std::vector<std::pair<std::string, long>> items;
            while (struct dirent *ent = readdir(d)) {
                std::string nm = ent->d_name;
                if (!script_name_ok(nm)) continue;
                struct stat st;
                long sz = (stat((host + "/" + nm).c_str(), &st) == 0 && S_ISREG(st.st_mode)) ? (long)st.st_size : -1;
                if (sz >= 0) items.push_back({nm, sz});
            }
            closedir(d);
            std::sort(items.begin(), items.end());
            for (size_t i = 0; i < items.size(); i++) {
                if (i) out += ',';
                out += "{\"name\":";
                web_json_str(out, items[i].first.c_str());
                out += ",\"size\":" + tools_int(items[i].second) + "}";
            }
        }
    }
    out += "]}";
}

static bool scripts_handle(web_req_t *r)
{
    if (tools_is(r, "POST", "/api/script/run")) {
        std::string body;
        if (!tools_read_json(r, body, SCRIPT_SRC_MAX + 1024)) return true;
        std::string src, cwd;
        web_json_get_str(body, "source", src);
        web_json_get_str(body, "cwd", cwd);
        if (cwd.empty()) cwd = "/sd";
        if (src.size() > SCRIPT_SRC_MAX) { web_resp_error(r, 400, "too_large", "script too large"); return true; }
        std::string out;
        script_run(cwd, src, out);
        std::string j = "{\"cwd\":";
        web_json_str(j, cwd.c_str());
        j += ",\"output\":";
        web_json_str(j, out.c_str());
        j += "}";
        web_resp_json(r, 200, j);
        return true;
    }

    if (tools_is(r, "GET", "/api/script/list")) {
        std::string j;
        scripts_list(j);
        web_resp_json(r, 200, j);
        return true;
    }

    if (tools_is(r, "GET", "/api/script/read")) {
        char namebuf[64] = {0};
        web_query_get(r, "name", namebuf, sizeof(namebuf));
        std::string name = namebuf;
        if (!script_name_ok(name)) { web_resp_error(r, 400, "bad_name", "bad script name"); return true; }
        std::string host;
        if (web_fs_resolve(std::string(SCRIPT_DIR "/" + name).c_str(), host)) { web_resp_error(r, 503, "sd_missing", "SD card not mounted"); return true; }
        FILE *f = fopen(host.c_str(), "rb");
        if (!f) { web_resp_error(r, 404, "not_found", "no such script"); return true; }
        std::string src;
        char chunk[512];
        size_t rd;
        while ((rd = fread(chunk, 1, sizeof(chunk), f)) > 0) {
            src.append(chunk, rd);
            if (src.size() > SCRIPT_SRC_MAX) { src.resize(SCRIPT_SRC_MAX); break; }
        }
        fclose(f);
        std::string j = "{\"name\":";
        web_json_str(j, name.c_str());
        j += ",\"source\":";
        web_json_str(j, src.c_str());
        j += "}";
        web_resp_json(r, 200, j);
        return true;
    }

    if (tools_is(r, "POST", "/api/script/save")) {
        std::string body;
        if (!tools_read_json(r, body, SCRIPT_SRC_MAX + 1024)) return true;
        std::string name, src;
        web_json_get_str(body, "name", name);
        web_json_get_str(body, "source", src);
        if (!script_name_ok(name)) { web_resp_error(r, 400, "bad_name", "bad script name"); return true; }
        if (src.size() > SCRIPT_SRC_MAX) { web_resp_error(r, 400, "too_large", "script too large"); return true; }
        std::string dirhost;
        if (web_fs_resolve(SCRIPT_DIR, dirhost)) { web_resp_error(r, 503, "sd_missing", "SD card not mounted"); return true; }
        mkdir(dirhost.c_str(), 0777);                  /* harmless if it already exists */
        std::string host;
        web_fs_resolve(std::string(SCRIPT_DIR "/" + name).c_str(), host);
        FILE *f = fopen(host.c_str(), "wb");
        if (!f) { web_resp_error(r, 500, "write_failed", "could not write script"); return true; }
        if (!src.empty()) fwrite(src.data(), 1, src.size(), f);
        fclose(f);
        std::string j = "{\"ok\":true,\"name\":";
        web_json_str(j, name.c_str());
        j += ",\"size\":" + tools_int((long long)src.size()) + "}";
        web_resp_json(r, 200, j);
        return true;
    }

    if (tools_is(r, "POST", "/api/script/delete")) {
        std::string body;
        if (!tools_read_json(r, body, 512)) return true;
        std::string name;
        web_json_get_str(body, "name", name);
        if (!script_name_ok(name)) { web_resp_error(r, 400, "bad_name", "bad script name"); return true; }
        std::string host;
        if (web_fs_resolve(std::string(SCRIPT_DIR "/" + name).c_str(), host)) { web_resp_error(r, 503, "sd_missing", "SD card not mounted"); return true; }
        if (remove(host.c_str()) != 0) { web_resp_error(r, 404, "not_found", "no such script"); return true; }
        web_resp_ok(r);
        return true;
    }

    return false;
}

static const char *const SCRIPT_CAPS[] = { "script", NULL };
const web_tool_t web_tool_script = { SCRIPT_CAPS, scripts_handle, NULL, NULL, NULL };

#endif /* FACTORY_WEB */
