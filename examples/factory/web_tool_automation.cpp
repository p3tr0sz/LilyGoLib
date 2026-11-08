/**
 * @file      web_tool_automation.cpp
 * @brief     The Automation app: a real, embeddable scripting language on the watch, as a *separate*
 *            app from the sh-like Scripts DSL (web_tool_script.cpp). Where the DSL is a shell, this is
 *            a full language — functions, data structures, a standard library.
 *
 * The engine is **Berry** (MIT, github.com/berry-lang/berry — the interpreter Tasmota embeds on the
 * ESP32), vendored under examples/factory/berry/. It is built the supported way: compiled as C with
 * BE_USE_PRECOMPILED_OBJECT 1 and the coc-generated constant tables in berry/generate/ (regenerate
 * with: python3 tools/coc/coc -o generate src default -c default/berry_conf.h from a Berry checkout).
 * mJS was rejected on licence grounds (GPLv2); see docs/handoff-scripting.md.
 *
 * This first slice exposes only the language and `print` — enough to prove the engine runs end to end
 * and to measure its footprint. A later slice binds a safe device API (`sys.*`, sandboxed `fs.*`,
 * notify, and radio/attack verbs behind the P9 claim gates), a script library on /sd, and the web +
 * native front-ends. The JS mock cannot re-run a native interpreter, so this is validated against the
 * simulator/firmware directly (unlike the DSL, which the mock mirrors).
 *
 * A run is bounded: the source and output are capped, and a wall-clock budget is enforced from Berry's
 * VM heartbeat so a runaway loop cannot hang the device. Not re-entrant — the web server runs one
 * request at a time, and the VM is created and destroyed per run.
 */
#include "web_tools.h"

/* Disconnected 2026-11-08 pending the headless rule-engine redesign. Gated off by default so the Berry
 * interpreter (~133 KB flash) is not referenced and --gc-sections drops it; Berry's files stay in-tree.
 * Define FACTORY_AUTOMATION to re-enable (also restore &web_tool_automation in web_tools.cpp). */
#if FACTORY_WEB && defined(FACTORY_AUTOMATION)

#include "web_internal.h"
#include <string>
#include <string.h>
#include <stdarg.h>

#include "berry/src/berry.h"                           /* berry.h carries its own extern "C" guard */

/* Defined in berry/src/be_port.c (local change): print output goes here when set, else to stdout. */
extern "C" void (*berry_output_hook)(const char *buffer, size_t length);

#define AUTO_SRC_MAX   (16 * 1024)                     /* largest source accepted */
#define AUTO_OUT_MAX   (32 * 1024)                     /* cap on captured output */
#define AUTO_MS_MAX    8000                            /* wall-clock budget for one run, ms */

/* ---- output capture: Berry's be_writebuffer() calls this through berry_output_hook ---- */
static std::string *g_auto_out = NULL;
static void auto_out_cb(const char *buf, size_t len)
{
    if (g_auto_out && g_auto_out->size() < AUTO_OUT_MAX) g_auto_out->append(buf, len);
}

/* ---- time guard: the VM fires BE_OBS_VM_HEARTBEAT periodically; abort if over budget ---- */
static uint32_t g_auto_start = 0;
static void auto_obs_hook(bvm *vm, int event, ...)
{
    if (event == BE_OBS_VM_HEARTBEAT && tools_now_ms() - g_auto_start > AUTO_MS_MAX)
        be_raise(vm, "runtime_error", "time limit exceeded");
}

/* Run a Berry source string, appending its output (and any error/traceback) to out. Returns true if
 * the script compiled and ran without an uncaught exception. */
static bool berry_run(const std::string &src, std::string &out)
{
    bvm *vm = be_vm_new();
    if (!vm) { out += "automation: out of memory\n"; return false; }

    g_auto_out = &out;
    g_auto_start = tools_now_ms();
    berry_output_hook = auto_out_cb;
    be_set_obs_hook(vm, auto_obs_hook);

    bool ok = true;
    int res = be_loadstring(vm, src.c_str());
    if (res == BE_OK) {
        res = be_pcall(vm, 0);                         /* execute the compiled chunk */
        if (res != BE_OK) { be_dumpexcept(vm); ok = false; }   /* traceback goes through the hook */
    } else {
        out += "compile error: ";
        out += (be_isstring(vm, -1) ? be_tostring(vm, -1) : "syntax error");
        out += "\n";
        ok = false;
    }

    berry_output_hook = NULL;                           /* unhook before anything else can print */
    g_auto_out = NULL;
    be_vm_delete(vm);
    return ok;
}

/* ---------------------------------------------------------------- web endpoint */

static bool automation_handle(web_req_t *r)
{
    if (!tools_is(r, "POST", "/api/automation/run")) return false;
    std::string body;
    if (!tools_read_json(r, body, AUTO_SRC_MAX + 1024)) return true;
    std::string src;
    web_json_get_str(body, "source", src);
    if (src.size() > AUTO_SRC_MAX) { web_resp_error(r, 400, "too_large", "script too large"); return true; }
    std::string out;
    bool ok = berry_run(src, out);
    std::string j = "{\"ok\":";
    j += ok ? "true" : "false";
    j += ",\"engine\":\"berry\",\"output\":";
    web_json_str(j, out.c_str());
    j += "}";
    web_resp_json(r, 200, j);
    return true;
}

static const char *const AUTO_CAPS[] = { "automation", NULL };
const web_tool_t web_tool_automation = { AUTO_CAPS, automation_handle, NULL, NULL, NULL };

#endif /* FACTORY_WEB */
