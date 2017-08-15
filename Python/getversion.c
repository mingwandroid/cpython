
/* Return the full version string. */

#include "Python.h"
#include "osdefs.h"
#include "patchlevel.h"


const char *
Anaconda_GetVersion(void)
{
	#ifdef MS_WIN32
	#define STDLIB_DIR  L"\\Lib\\"
	#else
	#define STDLIB_DIR  L"/lib/python3.6/"
	#endif
	static char res[128];
	FILE *f;
	wchar_t path[MAXPATHLEN+1];
	int c, i;

	wcscpy(path, Py_GetPrefix());
	wcscat(path, STDLIB_DIR L"version.txt");

	f = _Py_wfopen(path, L"rb");
	if (f == NULL) {
		strcpy(res, "Anaconda, Inc.");
	}
	else {
		i = 0;
		while (i + 1 < sizeof(res)) {
		c = fgetc(f);
			if (c == EOF || c == '\n' || c == '\r')
				break;
			res[i++] = c;
		}
		fclose(f);
		res[i] = '\0';
	}
	return res;
	#undef STDLIB_DIR
}


const char *
Py_GetVersion(void)
{
	static char version[250];
	PyOS_snprintf(version, sizeof(version), "%.80s |%s| (%.80s) %.80s",
		      PY_VERSION, Anaconda_GetVersion(),
		      Py_GetBuildInfo(), Py_GetCompiler());
	return version;
}
