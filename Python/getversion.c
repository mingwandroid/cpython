
/* Return the full version string. */

#include "Python.h"
#include "osdefs.h"

#include "patchlevel.h"


const char *
Anaconda_GetVersion(void)
{
#ifdef MS_WIN32
    #define STDLIB_DIR  L"\\Lib\\"
    /* We want to allow paths > 260 characters on Windows some day. */
    #define MPL 2048
#else
    #define STR(_s) #_s
    #define XSTR(_s) STR(_s)
    #define STDLIB_DIR L"/lib/python" XSTR(PY_MAJOR_VERSION) L"." XSTR(PY_MINOR_VERSION) L"/"
    #define MPL MAXPATHLEN
#endif

    static char res[128];
    FILE *f;
    wchar_t path[MPL + 1];
    int c;
    unsigned int i;

    wcscpy(path, Py_GetPrefix());
    wcscat(path, STDLIB_DIR L"version.txt");

    f = _Py_wfopen(path, L"rb");
    if (f == NULL)
        strcpy(res, "Anaconda, Inc.");
    else {
        i = 0;
        while (i < sizeof(res) - 1) {
            c = fgetc(f);
            if (c == EOF || c == '\n' || c == '\r')
                break;
            res[i++] = c;
        }
        fclose(f);
        res[i] = '\0';
    }
    return res;
    #undef STR
    #undef XSTR
    #undef STDLIB_DIR
    #undef MPL
}


const char *
Py_GetVersion(void)
{
    static char version[250];
    PyOS_snprintf(version, sizeof(version), "%.80s (%.80s) %.80s",
                  PY_VERSION, Py_GetBuildInfo(), Py_GetCompiler());
    return version;
}
