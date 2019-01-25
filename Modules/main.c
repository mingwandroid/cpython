/* Python interpreter main program */

#include "Python.h"
#include "pycore_initconfig.h"
#include "pycore_pylifecycle.h"
#include "pycore_pymem.h"
#include "pycore_pystate.h"

#ifdef __FreeBSD__
#  include <fenv.h>     /* fedisableexcept() */
#endif

/* Includes for exit_sigint() */
#include <stdio.h>      /* perror() */
#ifdef HAVE_SIGNAL_H
#  include <signal.h>   /* SIGINT */
#endif
#if defined(HAVE_GETPID) && defined(HAVE_UNISTD_H)
#  include <unistd.h>   /* getpid() */
#endif
#ifdef MS_WINDOWS
#  include <windows.h>  /* STATUS_CONTROL_C_EXIT */
#  include <shlwapi.h>
#  include <string.h>
#  include <malloc.h>
#  include <libloaderapi.h>
#endif
/* End of includes for exit_sigint() */

#define COPYRIGHT \
    "Type \"help\", \"copyright\", \"credits\" or \"license\" " \
    "for more information."

#ifdef __cplusplus
extern "C" {
#endif

extern char *CondaEcosystemGetWarnings();
extern int CondaEcosystemModifyDllSearchPath_Init(int argc, wchar_t *argv[]);

/* --- pymain_init() ---------------------------------------------- */

static PyStatus
pymain_init(const _PyArgv *args)
{
    PyStatus status;

#ifdef MS_WINDOWS
    /* LoadAndUnloadTestDLL(L"libiomp5md.dll"); */
    CondaEcosystemModifyDllSearchPath_Init(pymain->argc, pymain->wchar_argv);
    /* LoadAndUnloadTestDLL(L"libiomp5md.dll"); */
#endif
    status = _PyRuntime_Initialize();
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    /* 754 requires that FP exceptions run in "no stop" mode by default,
     * and until C vendors implement C99's ways to control FP exceptions,
     * Python requires non-stop mode.  Alas, some platforms enable FP
     * exceptions by default.  Here we disable them.
     */
#ifdef __FreeBSD__
    fedisableexcept(FE_OVERFLOW);
#endif

    PyPreConfig preconfig;
    PyPreConfig_InitPythonConfig(&preconfig);
    status = _Py_PreInitializeFromPyArgv(&preconfig, args);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    PyConfig config;
    status = PyConfig_InitPythonConfig(&config);
    if (_PyStatus_EXCEPTION(status)) {
        goto done;
    }

    /* pass NULL as the config: config is read from command line arguments,
       environment variables, configuration files */
    if (args->use_bytes_argv) {
        status = PyConfig_SetBytesArgv(&config, args->argc, args->bytes_argv);
    }
    else {
        status = PyConfig_SetArgv(&config, args->argc, args->wchar_argv);
    }
    if (_PyStatus_EXCEPTION(status)) {
        goto done;
    }

    status = Py_InitializeFromConfig(&config);
    if (_PyStatus_EXCEPTION(status)) {
        goto done;
    }
    status = _PyStatus_OK();

done:
    PyConfig_Clear(&config);
    return status;
}


/* --- pymain_run_python() ---------------------------------------- */

/* Non-zero if filename, command (-c) or module (-m) is set
   on the command line */
static inline int config_run_code(const PyConfig *config)
{
    return (config->run_command != NULL
            || config->run_filename != NULL
            || config->run_module != NULL);
}


/* Return non-zero is stdin is a TTY or if -i command line option is used */
static int
stdin_is_interactive(const PyConfig *config)
{
    return (isatty(fileno(stdin)) || config->interactive);
}


/* Display the current Python exception and return an exitcode */
static int
pymain_err_print(int *exitcode_p)
{
    int exitcode;
    if (_Py_HandleSystemExit(&exitcode)) {
        *exitcode_p = exitcode;
        return 1;
    }

    PyErr_Print();
    return 0;
}


static int
pymain_exit_err_print(void)
{
    int exitcode = 1;
    pymain_err_print(&exitcode);
    return exitcode;
}


/* Write an exitcode into *exitcode and return 1 if we have to exit Python.
   Return 0 otherwise. */
static int
pymain_get_importer(const wchar_t *filename, PyObject **importer_p, int *exitcode)
{
    PyObject *sys_path0 = NULL, *importer;

    sys_path0 = PyUnicode_FromWideChar(filename, wcslen(filename));
    if (sys_path0 == NULL) {
        goto error;
    }

    importer = PyImport_GetImporter(sys_path0);
    if (importer == NULL) {
        goto error;
    }

    if (importer == Py_None) {
        Py_DECREF(sys_path0);
        Py_DECREF(importer);
        return 0;
    }

    Py_DECREF(importer);
    *importer_p = sys_path0;
    return 0;

error:
    Py_XDECREF(sys_path0);

    PySys_WriteStderr("Failed checking if argv[0] is an import path entry\n");
    return pymain_err_print(exitcode);
}


static int
pymain_sys_path_add_path0(PyInterpreterState *interp, PyObject *path0)
{
    _Py_IDENTIFIER(path);
    PyObject *sys_path;
    PyObject *sysdict = interp->sysdict;
    if (sysdict != NULL) {
        sys_path = _PyDict_GetItemIdWithError(sysdict, &PyId_path);
        if (sys_path == NULL && PyErr_Occurred()) {
            return -1;
        }
    }
    else {
        sys_path = NULL;
    }
    if (sys_path == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "unable to get sys.path");
        return -1;
    }

    if (PyList_Insert(sys_path, 0, path0)) {
        return -1;
    }
    return 0;
}


static void
pymain_header(const PyConfig *config)
{
    if (config->quiet) {
        return;
    }

    if (!config->verbose && (config_run_code(config) || !stdin_is_interactive(config))) {
        return;
    }

    fprintf(stderr, "Python %s :: %s on %s\n%s", Py_GetVersion(), Anaconda_GetVersion(), Py_GetPlatform(), CondaEcosystemGetWarnings());
    if (config->site_import) {
        fprintf(stderr, "%s\n", COPYRIGHT);
    }
}


static void
pymain_import_readline(const PyConfig *config)
{
    if (config->isolated) {
        return;
    }
    if (!config->inspect && config_run_code(config)) {
        return;
    }
    if (!isatty(fileno(stdin))) {
        return;
    }

    PyObject *mod = PyImport_ImportModule("readline");
    if (mod == NULL) {
        PyErr_Clear();
    }
    else {
        Py_DECREF(mod);
    }
}


static int
pymain_run_command(wchar_t *command, PyCompilerFlags *cf)
{
    PyObject *unicode, *bytes;
    int ret;

    unicode = PyUnicode_FromWideChar(command, -1);
    if (unicode == NULL) {
        goto error;
    }

    if (PySys_Audit("cpython.run_command", "O", unicode) < 0) {
        return pymain_exit_err_print();
    }

    bytes = PyUnicode_AsUTF8String(unicode);
    Py_DECREF(unicode);
    if (bytes == NULL) {
        goto error;
    }

    ret = PyRun_SimpleStringFlags(PyBytes_AsString(bytes), cf);
    Py_DECREF(bytes);
    return (ret != 0);

error:
    PySys_WriteStderr("Unable to decode the command from the command line:\n");
    return pymain_exit_err_print();
}


static int
pymain_run_module(const wchar_t *modname, int set_argv0)
{
    PyObject *module, *runpy, *runmodule, *runargs, *result;
    if (PySys_Audit("cpython.run_module", "u", modname) < 0) {
        return pymain_exit_err_print();
    }
    runpy = PyImport_ImportModule("runpy");
    if (runpy == NULL) {
        fprintf(stderr, "Could not import runpy module\n");
        return pymain_exit_err_print();
    }
    runmodule = PyObject_GetAttrString(runpy, "_run_module_as_main");
    if (runmodule == NULL) {
        fprintf(stderr, "Could not access runpy._run_module_as_main\n");
        Py_DECREF(runpy);
        return pymain_exit_err_print();
    }
    module = PyUnicode_FromWideChar(modname, wcslen(modname));
    if (module == NULL) {
        fprintf(stderr, "Could not convert module name to unicode\n");
        Py_DECREF(runpy);
        Py_DECREF(runmodule);
        return pymain_exit_err_print();
    }
    runargs = Py_BuildValue("(Oi)", module, set_argv0);
    if (runargs == NULL) {
        fprintf(stderr,
            "Could not create arguments for runpy._run_module_as_main\n");
        Py_DECREF(runpy);
        Py_DECREF(runmodule);
        Py_DECREF(module);
        return pymain_exit_err_print();
    }
    result = PyObject_Call(runmodule, runargs, NULL);
    Py_DECREF(runpy);
    Py_DECREF(runmodule);
    Py_DECREF(module);
    Py_DECREF(runargs);
    if (result == NULL) {
        return pymain_exit_err_print();
    }
    Py_DECREF(result);
    return 0;
}


static int
pymain_run_file(PyConfig *config, PyCompilerFlags *cf)
{
    const wchar_t *filename = config->run_filename;
    if (PySys_Audit("cpython.run_file", "u", filename) < 0) {
        return pymain_exit_err_print();
    }
    FILE *fp = _Py_wfopen(filename, L"rb");
    if (fp == NULL) {
        char *cfilename_buffer;
        const char *cfilename;
        int err = errno;
        cfilename_buffer = _Py_EncodeLocaleRaw(filename, NULL);
        if (cfilename_buffer != NULL)
            cfilename = cfilename_buffer;
        else
            cfilename = "<unprintable file name>";
        fprintf(stderr, "%ls: can't open file '%s': [Errno %d] %s\n",
                config->program_name, cfilename, err, strerror(err));
        PyMem_RawFree(cfilename_buffer);
        return 2;
    }

    if (config->skip_source_first_line) {
        int ch;
        /* Push back first newline so line numbers remain the same */
        while ((ch = getc(fp)) != EOF) {
            if (ch == '\n') {
                (void)ungetc(ch, fp);
                break;
            }
        }
    }

    struct _Py_stat_struct sb;
    if (_Py_fstat_noraise(fileno(fp), &sb) == 0 && S_ISDIR(sb.st_mode)) {
        fprintf(stderr,
                "%ls: '%ls' is a directory, cannot continue\n",
                config->program_name, filename);
        fclose(fp);
        return 1;
    }

    /* call pending calls like signal handlers (SIGINT) */
    if (Py_MakePendingCalls() == -1) {
        fclose(fp);
        return pymain_exit_err_print();
    }

    PyObject *unicode, *bytes = NULL;
    const char *filename_str;

    unicode = PyUnicode_FromWideChar(filename, wcslen(filename));
    if (unicode != NULL) {
        bytes = PyUnicode_EncodeFSDefault(unicode);
        Py_DECREF(unicode);
    }
    if (bytes != NULL) {
        filename_str = PyBytes_AsString(bytes);
    }
    else {
        PyErr_Clear();
        filename_str = "<filename encoding error>";
    }

    /* PyRun_AnyFileExFlags(closeit=1) calls fclose(fp) before running code */
    int run = PyRun_AnyFileExFlags(fp, filename_str, 1, cf);
    Py_XDECREF(bytes);
    return (run != 0);
}


static int
pymain_run_startup(PyConfig *config, PyCompilerFlags *cf, int *exitcode)
{
    const char *startup = _Py_GetEnv(config->use_environment, "PYTHONSTARTUP");
    if (startup == NULL) {
        return 0;
    }
    if (PySys_Audit("cpython.run_startup", "s", startup) < 0) {
        return pymain_err_print(exitcode);
    }

    FILE *fp = _Py_fopen(startup, "r");
    if (fp == NULL) {
        int save_errno = errno;
        PySys_WriteStderr("Could not open PYTHONSTARTUP\n");

        errno = save_errno;
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, startup);

        return pymain_err_print(exitcode);
    }

    (void) PyRun_SimpleFileExFlags(fp, startup, 0, cf);
    PyErr_Clear();
    fclose(fp);
    return 0;
}


/* Write an exitcode into *exitcode and return 1 if we have to exit Python.
   Return 0 otherwise. */
static int
pymain_run_interactive_hook(int *exitcode)
{
    PyObject *sys, *hook, *result;
    sys = PyImport_ImportModule("sys");
    if (sys == NULL) {
        goto error;
    }

    hook = PyObject_GetAttrString(sys, "__interactivehook__");
    Py_DECREF(sys);
    if (hook == NULL) {
        PyErr_Clear();
        return 0;
    }

    if (PySys_Audit("cpython.run_interactivehook", "O", hook) < 0) {
        goto error;
    }

    result = _PyObject_CallNoArg(hook);
    Py_DECREF(hook);
    if (result == NULL) {
        goto error;
    }
    Py_DECREF(result);

    return 0;

error:
    PySys_WriteStderr("Failed calling sys.__interactivehook__\n");
    return pymain_err_print(exitcode);
}


static int
pymain_run_stdin(PyConfig *config, PyCompilerFlags *cf)
{
    if (stdin_is_interactive(config)) {
        config->inspect = 0;
        Py_InspectFlag = 0; /* do exit on SystemExit */

        int exitcode;
        if (pymain_run_startup(config, cf, &exitcode)) {
            return exitcode;
        }

        if (pymain_run_interactive_hook(&exitcode)) {
            return exitcode;
        }
    }

    /* call pending calls like signal handlers (SIGINT) */
    if (Py_MakePendingCalls() == -1) {
        return pymain_exit_err_print();
    }

    if (PySys_Audit("cpython.run_stdin", NULL) < 0) {
        return pymain_exit_err_print();
    }

    int run = PyRun_AnyFileExFlags(stdin, "<stdin>", 0, cf);
    return (run != 0);
}


static void
pymain_repl(PyConfig *config, PyCompilerFlags *cf, int *exitcode)
{
    /* Check this environment variable at the end, to give programs the
       opportunity to set it from Python. */
    if (!config->inspect && _Py_GetEnv(config->use_environment, "PYTHONINSPECT")) {
        config->inspect = 1;
        Py_InspectFlag = 1;
    }

    if (!(config->inspect && stdin_is_interactive(config) && config_run_code(config))) {
        return;
    }

    config->inspect = 0;
    Py_InspectFlag = 0;
    if (pymain_run_interactive_hook(exitcode)) {
        return;
    }

    int res = PyRun_AnyFileFlags(stdin, "<stdin>", cf);
    *exitcode = (res != 0);
}


static void
pymain_run_python(int *exitcode)
{
    PyInterpreterState *interp = _PyInterpreterState_GET_UNSAFE();
    /* pymain_run_stdin() modify the config */
    PyConfig *config = &interp->config;

    PyObject *main_importer_path = NULL;
    if (config->run_filename != NULL) {
        /* If filename is a package (ex: directory or ZIP file) which contains
           __main__.py, main_importer_path is set to filename and will be
           prepended to sys.path.

           Otherwise, main_importer_path is left unchanged. */
        if (pymain_get_importer(config->run_filename, &main_importer_path,
                                exitcode)) {
            return;
        }
    }

    if (main_importer_path != NULL) {
        if (pymain_sys_path_add_path0(interp, main_importer_path) < 0) {
            goto error;
        }
    }
    else if (!config->isolated) {
        PyObject *path0 = NULL;
        int res = _PyPathConfig_ComputeSysPath0(&config->argv, &path0);
        if (res < 0) {
            goto error;
        }

        if (res > 0) {
            if (pymain_sys_path_add_path0(interp, path0) < 0) {
                Py_DECREF(path0);
                goto error;
            }
            Py_DECREF(path0);
        }
    }

    PyCompilerFlags cf = _PyCompilerFlags_INIT;

    pymain_header(config);
    pymain_import_readline(config);

    if (config->run_command) {
        *exitcode = pymain_run_command(config->run_command, &cf);
    }
    else if (config->run_module) {
        *exitcode = pymain_run_module(config->run_module, 1);
    }
    else if (main_importer_path != NULL) {
        *exitcode = pymain_run_module(L"__main__", 0);
    }
    else if (config->run_filename != NULL) {
        *exitcode = pymain_run_file(config, &cf);
    }
    else {
        *exitcode = pymain_run_stdin(config, &cf);
    }

    pymain_repl(config, &cf, exitcode);
    goto done;

error:
    *exitcode = pymain_exit_err_print();

done:
    Py_XDECREF(main_importer_path);
}


/* --- pymain_main() ---------------------------------------------- */

static void
pymain_free(void)
{
    _PyImport_Fini2();

    /* Free global variables which cannot be freed in Py_Finalize():
       configuration options set before Py_Initialize() which should
       remain valid after Py_Finalize(), since
       Py_Initialize()-Py_Finalize() can be called multiple times. */
    _PyPathConfig_ClearGlobal();
    _Py_ClearStandardStreamEncoding();
    _Py_ClearArgcArgv();
    _PyRuntime_Finalize();
}


static int
exit_sigint(void)
{
    /* bpo-1054041: We need to exit via the
     * SIG_DFL handler for SIGINT if KeyboardInterrupt went unhandled.
     * If we don't, a calling process such as a shell may not know
     * about the user's ^C.  https://www.cons.org/cracauer/sigint.html */
#if defined(HAVE_GETPID) && !defined(MS_WINDOWS)
    if (PyOS_setsig(SIGINT, SIG_DFL) == SIG_ERR) {
        perror("signal");  /* Impossible in normal environments. */
    } else {
        kill(getpid(), SIGINT);
    }
    /* If setting SIG_DFL failed, or kill failed to terminate us,
     * there isn't much else we can do aside from an error code. */
#endif  /* HAVE_GETPID && !MS_WINDOWS */
#ifdef MS_WINDOWS
    /* cmd.exe detects this, prints ^C, and offers to terminate. */
    /* https://msdn.microsoft.com/en-us/library/cc704588.aspx */
    return STATUS_CONTROL_C_EXIT;
#else
    return SIGINT + 128;
#endif  /* !MS_WINDOWS */
}


static void _Py_NO_RETURN
pymain_exit_error(PyStatus status)
{
    if (_PyStatus_IS_EXIT(status)) {
        /* If it's an error rather than a regular exit, leave Python runtime
           alive: Py_ExitStatusException() uses the current exception and use
           sys.stdout in this case. */
        pymain_free();
    }
    Py_ExitStatusException(status);
}


int
Py_RunMain(void)
{
    int exitcode = 0;

    pymain_run_python(&exitcode);

    if (Py_FinalizeEx() < 0) {
        /* Value unlikely to be confused with a non-error exit status or
           other special meaning */
        exitcode = 120;
    }

    pymain_free();

    if (_Py_UnhandledKeyboardInterrupt) {
        exitcode = exit_sigint();
    }

    return exitcode;
}


#ifdef MS_WINDOWS
/* Please do not remove this function. It is needed for testing
   CondaEcosystemModifyDllSearchPath(). */

/*
void LoadAndUnloadTestDLL(wchar_t* test_dll)
{
    wchar_t test_path[MAX_PATH + 1];
    HMODULE hDLL = LoadLibraryExW(&test_dll[0], NULL, 0);
    if (hDLL == NULL)
    {
        wchar_t err_msg[256];
        DWORD err_code = GetLastError();
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, err_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            err_msg, (sizeof(err_msg) / sizeof(wchar_t)), NULL);
        fwprintf(stderr, L"LoadAndUnloadTestDLL() :: ERROR :: Failed to load %ls, error is: %ls\n", &test_dll[0], &err_msg[0]);
    }
    GetModuleFileNameW(hDLL, &test_path[0], MAX_PATH);
    fwprintf(stderr, L"LoadAndUnloadTestDLL() :: %ls loaded from %ls\n", &test_dll[0], &test_path[0]);
    if (FreeLibrary(hDLL) == 0)
    {
        wchar_t err_msg[256];
        DWORD err_code = GetLastError();
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, err_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            err_msg, (sizeof(err_msg) / sizeof(wchar_t)), NULL);
        fwprintf(stderr, L"LoadAndUnloadTestDLL() :: ERROR :: Failed to free %ls, error is: %ls\n", &test_dll[0], &err_msg[0]);
    }
}
*/

/*
    Provided CONDA_DLL_SEARCH_MODIFICATION_ENABLE is set (to anything at all!)
    this function will modify the DLL search path so that C:\Windows\System32
    does not appear before entries in PATH. If it does appear in PATH then it
    gets added at the position it was in in PATH.

    This is achieved via a call to SetDefaultDllDirectories() then calls to
    AddDllDirectory() for each entry in PATH. We also take the opportunity to
    clean-up these PATH entries such that any '/' are replaced with '\', no
    double quotes occour and no PATH entry ends with '\'.

    Caution: Microsoft's documentation says that the search order of entries
    passed to AddDllDirectory is not respected and arbitrary. I do not think
    this will be the case but it is worth bearing in mind.
*/

#if !defined(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#endif

/* Caching of prior processed PATH environment */
static wchar_t *sv_path_env = NULL;
typedef void (WINAPI *SDDD)(DWORD DirectoryFlags);
typedef void (WINAPI *SDD)(PCWSTR SetDir);
typedef void (WINAPI *ADD)(PCWSTR NewDirectory);
static SDDD pSetDefaultDllDirectories = NULL;
static SDD pSetDllDirectory = NULL;
static ADD pAddDllDirectory = NULL;
static int sv_failed_to_find_dll_fns = 0;
static int sv_conda_not_activated = 0;
/* sv_executable_dirname is gotten but not used ATM. */
static wchar_t sv_executable_dirname[1024];
/* Have hidden this behind a define because it is clearly not code that
   could be considered for upstreaming so clearly delimiting it makes it
   easier to remove. */
#define HARDCODE_CONDA_PATHS
#if defined(HARDCODE_CONDA_PATHS)
typedef struct
{
    wchar_t *p_relative;
    wchar_t *p_name;
} CONDA_PATH;

#define NUM_CONDA_PATHS 5

static CONDA_PATH condaPaths[NUM_CONDA_PATHS] =
{
    {L"Library\\mingw-w64\\bin", NULL},
    {L"Library\\usr\\bin", NULL},
    {L"Library\\bin", NULL},
    {L"Scripts", NULL},
    {L"bin", NULL}
};
#endif /* HARDCODE_CONDA_PATHS */
static wchar_t sv_executable_dirname[1024];
static wchar_t sv_windows_directory[1024];
static wchar_t *sv_added_windows_directory = NULL;
static wchar_t *sv_added_cwd = NULL;

int CondaEcosystemModifyDllSearchPath_Init(int argc, wchar_t *argv[])
{
    int debug_it = _wgetenv(L"CONDA_DLL_SEARCH_MODIFICATION_DEBUG") ? 1 : 0;
    int res = 0;
#if defined(HARDCODE_CONDA_PATHS)
    CONDA_PATH *p_conda_path;
#endif /* defined(HARDCODE_CONDA_PATHS) */

    if (pSetDefaultDllDirectories == NULL)
    {
        wchar_t *conda_prefix = _wgetenv(L"CONDA_PREFIX");
        wchar_t *build_prefix = _wgetenv(L"BUILD_PREFIX");
        wchar_t *prefix = _wgetenv(L"PREFIX");
        pSetDefaultDllDirectories = (SDDD)GetProcAddress(GetModuleHandle(TEXT("kernel32.dll")), "SetDefaultDllDirectories");
        pSetDllDirectory = (SDD)GetProcAddress(GetModuleHandle(TEXT("kernel32.dll")), "SetDllDirectoryW");
        pAddDllDirectory = (ADD)GetProcAddress(GetModuleHandle(TEXT("kernel32.dll")), "AddDllDirectory");

        /* Determine sv_executable_dirname */
        GetModuleFileNameW(NULL, &sv_executable_dirname[0], sizeof(sv_executable_dirname)/sizeof(sv_executable_dirname[0])-1);
        sv_executable_dirname[sizeof(sv_executable_dirname)/sizeof(sv_executable_dirname[0])-1] = L'\0';
        if (wcsrchr(sv_executable_dirname, L'\\'))
            *wcsrchr(sv_executable_dirname, L'\\') = L'\0';
#if defined(HARDCODE_CONDA_PATHS)
        for (p_conda_path = &condaPaths[0]; p_conda_path < &condaPaths[NUM_CONDA_PATHS]; ++p_conda_path)
        {
            size_t n_chars_executable_dirname = wcslen(sv_executable_dirname);
            size_t n_chars_p_relative = wcslen(p_conda_path->p_relative);
            p_conda_path->p_name = malloc(sizeof(wchar_t) * (n_chars_executable_dirname + n_chars_p_relative + 2));
            wcsncpy(p_conda_path->p_name, sv_executable_dirname, n_chars_executable_dirname+1);
            wcsncat(p_conda_path->p_name, L"\\", 2);
            wcsncat(p_conda_path->p_name, p_conda_path->p_relative, n_chars_p_relative+1);
        }
#endif /* defined(HARDCODE_CONDA_PATHS) */

        /* Determine sv_windows_directory */
        {
            char tmp_ascii[1024];
            size_t convertedChars = 0;
            GetWindowsDirectory(&tmp_ascii[0], sizeof(tmp_ascii) / sizeof(tmp_ascii[0]) - 1);
            tmp_ascii[sizeof(tmp_ascii) / sizeof(tmp_ascii[0]) - 1] = L'\0';
            mbstowcs_s(&convertedChars, sv_windows_directory, strlen(tmp_ascii)+1, tmp_ascii, _TRUNCATE);
            sv_windows_directory[sizeof(sv_windows_directory) / sizeof(sv_windows_directory[0]) - 1] = L'\0';
        }

        if (conda_prefix == NULL || wcscmp(sv_executable_dirname, conda_prefix))
        {
            if (build_prefix == NULL || wcscmp(sv_executable_dirname, build_prefix))
            {
                if (prefix == NULL || wcscmp(sv_executable_dirname, prefix))
                {
                    int found_conda = 0;
                    int argi;
                    /* If any of the args contain 'conda' .. I am very sorry and there's probably a better way. */
                    for (argi = 1; argi < argc; ++argi)
                    {
                        if (wcscmp(argv[argi], L"conda") == 0)
                        {
                            found_conda = 1;
                            break;
                        }
                    }
                    if (found_conda == 0)
                    {
                        sv_conda_not_activated = 1;
                        res = 1;
                    }
                }
            }
        }
    }

    if (pSetDefaultDllDirectories == NULL || pSetDllDirectory == NULL || pAddDllDirectory == NULL)
    {
        if (debug_it)
            fwprintf(stderr, L"CondaEcosystemModifyDllSearchPath() :: WARNING :: Please install KB2533623 from http://go.microsoft.com/fwlink/p/?linkid=217865\n"\
                             L"CondaEcosystemModifyDllSearchPath() :: WARNING :: to improve conda ecosystem DLL isolation");
        sv_failed_to_find_dll_fns = 1;
        res = 2;
    }
    return res;
}

char* CondaEcosystemGetWarnings()
{
    static char warnings[1024] = { 0 };
    if (sv_conda_not_activated == 1 && warnings[0] == '\0')
    {
        sprintf(warnings, "\n"
                          "Warning:\n"
                          "This Python interpreter is in a conda environment, but the environment has\n"
                          "not been activated.  Libraries may fail to load.  To activate this environment\n"
                          "please see https://conda.io/activation\n"
                          "\n");
    }
    return &warnings[0];
}

int CondaEcosystemModifyDllSearchPath(int add_windows_directory, int add_cwd) {
    /* Returns:

       -1: CONDA_DLL_SEARCH_MODIFICATION_ENABLE unset.
        0: Success (this can mean that no changes were made, i.e. no calls to the kernel functions were performed).
        1: Failed to find kernel functions necessary (i.e. your Windows is too old).
    */
    int debug_it = _wgetenv(L"CONDA_DLL_SEARCH_MODIFICATION_DEBUG") ? 1 : 0;
    const wchar_t *path_env = _wgetenv(L"PATH");
    wchar_t current_working_directory[1024];
    const wchar_t *p_cwd = NULL;
    ssize_t entry_num = 0;
    ssize_t i;
    wchar_t **path_entries;
    wchar_t *path_end;
    ssize_t num_entries = 1;
#if defined(HARDCODE_CONDA_PATHS)
    ssize_t j;
    CONDA_PATH *p_conda_path;
    int foundCondaPath[NUM_CONDA_PATHS] = {0, 0, 0, 0, 0};
#endif /* defined(HARDCODE_CONDA_PATHS) */

    int SetDllDirectoryValue = LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    if (sv_failed_to_find_dll_fns)
        return 1;

    if (_wgetenv(L"CONDA_DLL_SEARCH_MODIFICATION_ENABLE") == NULL)
        return -1;
    if (_wgetenv(L"CONDA_DLL_SEARCH_MODIFICATION_NEVER_ADD_WINDOWS_DIRECTORY"))
        add_windows_directory = 0;
    if (_wgetenv(L"CONDA_DLL_SEARCH_MODIFICATION_NEVER_ADD_CWD"))
        add_cwd = 0;

    if (add_cwd)
    {
        _wgetcwd(&current_working_directory[0], (sizeof(current_working_directory)/sizeof(current_working_directory[0])) - 1);
        current_working_directory[sizeof(current_working_directory)/sizeof(current_working_directory[0]) - 1] = L'\0';
        p_cwd = &current_working_directory[0];
    }

    /* cache path to avoid multiple adds */
    if (sv_path_env != NULL && path_env != NULL && !wcscmp(path_env, sv_path_env))
    {
        if ((add_windows_directory && sv_added_windows_directory != NULL) ||
            (!add_windows_directory && sv_added_windows_directory == NULL) )
        {
            if ((p_cwd == NULL && sv_added_cwd == NULL) ||
                p_cwd != NULL && sv_added_cwd != NULL && !wcscmp(p_cwd, sv_added_cwd))
            {
                if (_wgetenv(L"CONDA_DLL_SEARCH_MODIFICATION_NEVER_CACHE") == NULL)
                {
                    if (debug_it) fwprintf(stderr, L"CondaEcosystemModifyDllSearchPath() :: INFO :: Values unchanged\n");
                    return 0;
                }
            }
        }
    }
    /* Something has changed.
       Reset to default search order */
    pSetDllDirectory(NULL);

    if (sv_path_env != NULL)
    {
        free(sv_path_env);
    }
    sv_path_env = (path_env == NULL) ? NULL : _wcsdup(path_env);

    if (path_env != NULL)
    {
        size_t len = wcslen(path_env);
        wchar_t *path = (wchar_t *)alloca((len + 1) * sizeof(wchar_t));
        if (debug_it) fwprintf(stderr, L"CondaEcosystemModifyDllSearchPath() :: PATH=%ls\n\b", path_env);
        memcpy(path, path_env, (len + 1) * sizeof(wchar_t));
        /* Convert any / to \ */
        /* Replace slash with backslash */
        while ((path_end = wcschr(path, L'/')))
            *path_end = L'\\';
        /* Remove all double quotes */
        while ((path_end = wcschr(path, L'"')))
            memmove(path_end, path_end + 1, sizeof(wchar_t) * (len-- - (path_end - path)));
        /* Remove all leading and double ';' */
        while (*path == L';')
            memmove(path, path + 1, sizeof(wchar_t) * len--);
        while ((path_end = wcsstr(path, L";;")))
            memmove(path_end, path_end + 1, sizeof(wchar_t) * (len-- - (path_end - path)));
        /* Remove trailing ;'s */
        while(path[len-1] == L';')
            path[len-- - 1] = L'\0';

        if (len == 0)
            return 2;

        /* Count the number of path entries */
        path_end = path;
        while ((path_end = wcschr(path_end, L';')))
        {
            ++num_entries;
            ++path_end;
        }

        path_entries = (wchar_t **)alloca((num_entries) * sizeof(wchar_t *));
        path_end = wcschr(path, L';');

        if (getenv("CONDA_DLL_SET_DLL_DIRECTORY_VALUE") != NULL)
            SetDllDirectoryValue = atoi(getenv("CONDA_DLL_SET_DLL_DIRECTORY_VALUE"));
        pSetDefaultDllDirectories(SetDllDirectoryValue);
        while (path != NULL)
        {
            if (path_end != NULL)
            {
                *path_end = L'\0';
                /* Hygiene, no \ at the end */
                while (path_end > path && path_end[-1] == L'\\')
                {
                    --path_end;
                    *path_end = L'\0';
                }
            }
            if (wcslen(path) != 0)
                path_entries[entry_num++] = path;
            path = path_end;
            if (path != NULL)
            {
                while (*path == L'\0')
                    ++path;
                path_end = wcschr(path, L';');
            }
        }
        for (i = num_entries - 1; i > -1; --i)
        {
#if defined(HARDCODE_CONDA_PATHS)
            for (j = 0, p_conda_path = &condaPaths[0]; p_conda_path < &condaPaths[NUM_CONDA_PATHS]; ++j, ++p_conda_path)
            {
                if (!foundCondaPath[j] && !wcscmp(path_entries[i], p_conda_path->p_name))
                {
                    foundCondaPath[j] = 1;
                    break;
                }
            }
#endif /* defined(HARDCODE_CONDA_PATHS) */
            if (debug_it) fwprintf(stderr, L"CondaEcosystemModifyDllSearchPath() :: AddDllDirectory(%ls)\n", path_entries[i]);
            pAddDllDirectory(path_entries[i]);
        }
    }

#if defined(HARDCODE_CONDA_PATHS)
    if (_wgetenv(L"CONDA_DLL_SEARCH_MODIFICATION_DO_NOT_ADD_EXEPREFIX") == NULL)
    {
        for (j = NUM_CONDA_PATHS-1, p_conda_path = &condaPaths[NUM_CONDA_PATHS-1]; j > -1; --j, --p_conda_path)
        {
            if (debug_it) fwprintf(stderr, L"CondaEcosystemModifyDllSearchPath() :: p_conda_path->p_name = %ls, foundCondaPath[%zd] = %d\n", p_conda_path->p_name, j, foundCondaPath[j]);
            if (!foundCondaPath[j])
            {
                if (debug_it) fwprintf(stderr, L"CondaEcosystemModifyDllSearchPath() :: AddDllDirectory(%ls - ExePrefix)\n", p_conda_path->p_name);
                pAddDllDirectory(p_conda_path->p_name);
            }
        }
    }
#endif /* defined(HARDCODE_CONDA_PATHS) */

    if (p_cwd)
    {
        if (sv_added_cwd != NULL && wcscmp(p_cwd, sv_added_cwd))
        {
            free(sv_added_cwd);
        }
        sv_added_cwd = _wcsdup(p_cwd);
        if (debug_it) fwprintf(stderr, L"CondaEcosystemModifyDllSearchPath() :: AddDllDirectory(%ls - CWD)\n", sv_added_cwd);
        pAddDllDirectory(sv_added_cwd);
    }

    if (add_windows_directory)
    {
        sv_added_windows_directory = &sv_windows_directory[0];
        if (debug_it) fwprintf(stderr, L"CondaEcosystemModifyDllSearchPath() :: AddDllDirectory(%ls - WinDir)\n", sv_windows_directory);
        pAddDllDirectory(sv_windows_directory);
    }
    else
    {
        sv_added_windows_directory = NULL;
    }

    return 0;
}
#else
char* CondaEcosystemGetWarnings()
{
    return "";
}
#endif


static int
pymain_main(_PyArgv *args)
{
    PyStatus status = pymain_init(args);
    if (_PyStatus_IS_EXIT(status)) {
        pymain_free();
        return status.exitcode;
    }
    if (_PyStatus_EXCEPTION(status)) {
        pymain_exit_error(status);
    }

    return Py_RunMain();
}


int
Py_Main(int argc, wchar_t **argv)
{
    _PyArgv args = {
        .argc = argc,
        .use_bytes_argv = 0,
        .bytes_argv = NULL,
        .wchar_argv = argv};
    return pymain_main(&args);
}


int
Py_BytesMain(int argc, char **argv)
{
    _PyArgv args = {
        .argc = argc,
        .use_bytes_argv = 1,
        .bytes_argv = argv,
        .wchar_argv = NULL};
    return pymain_main(&args);
}

#ifdef __cplusplus
}
#endif
