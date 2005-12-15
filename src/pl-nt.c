/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        jan@swi.psy.uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2002, University of Amsterdam

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#if defined(__WINDOWS__) || defined(__WIN32__) || defined(WIN32)
#define _WIN32_WINNT 0x0400
#include <winsock2.h>
#include <windows.h>

#include "pl-incl.h"
#include "pl-utf8.h"
#include <crtdbg.h>
#include <process.h>
#include "pl-ctype.h"
#include <stdio.h>
#include <stdarg.h>
#include "pl-stream.h"
#include <process.h>
#include <winbase.h>


		 /*******************************
		 *	    MESSAGE BOX		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
There is no way to tell which subsystem   an app belongs too, except for
peeking in its executable-header. This is a bit too much ...
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
hasConsole(void)
{ HANDLE h;

  if ( GD->os.gui_app == FALSE )	/* has been set explicitly */
    succeed;

					/* I found a console */
  if ( (h = GetStdHandle(STD_OUTPUT_HANDLE)) != INVALID_HANDLE_VALUE )
  { DWORD mode;

    if ( GetConsoleMode(h, &mode) )
      succeed;
  }

					/* assume we are GUI */
  fail;
}


void
PlMessage(const char *fm, ...)
{ va_list(args);

  va_start(args, fm);
  
  if ( hasConsole() )
  { Sfprintf(Serror, "SWI-Prolog: ");
    Svfprintf(Serror, fm, args);
    Sfprintf(Serror, "\n");
  } else
  { char buf[1024];

    vsprintf(buf, fm, args);
    MessageBox(NULL, buf, "SWI-Prolog", MB_OK|MB_TASKMODAL);
  }

  va_end(args);
}

		 /*******************************
		 *	       WIN32		*
		 *******************************/

int
iswin32s()
{ if( GetVersion() & 0x80000000 && (GetVersion() & 0xFF) ==3)
    return TRUE;
  else
    return FALSE;
}


		 /*******************************
		 *	WinAPI ERROR CODES	*
		 *******************************/

char *
WinError()
{ int id = GetLastError();
  char *msg;
  static WORD lang;
  static lang_initialised = 0;

  if ( !lang_initialised )
    lang = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_UK);

again:
  if ( FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER|
		     FORMAT_MESSAGE_IGNORE_INSERTS|
		     FORMAT_MESSAGE_FROM_SYSTEM,
		     NULL,			/* source */
		     id,			/* identifier */
		     lang,
		     (LPTSTR) &msg,
		     0,				/* size */
		     NULL) )			/* arguments */
  { atom_t a = PL_new_atom(msg);

    LocalFree(msg);
    lang_initialised = 1;

    return stringAtom(a);
  } else
  { if ( lang_initialised == 0 )
    { lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);
      lang_initialised = 1;
      goto again;
    }

    return "Unknown Windows error";
  }
}


		 /*******************************
		 *	  SLEEP/1 SUPPORT	*
		 *******************************/

int
Pause(double t)
{ HANDLE h;

  if ( (h = CreateWaitableTimer(NULL, TRUE, NULL)) )
  { LARGE_INTEGER ft;

    ft.QuadPart = -(LONGLONG)(t * 10000000.0); /* 100 nanosecs per tick */

    SetWaitableTimer(h, &ft, 0, NULL, NULL, FALSE);
    for(;;)
    { int rc = MsgWaitForMultipleObjects(1,
					 &h,
					 FALSE,
					 INFINITE,
					 QS_ALLINPUT);
      if ( rc == WAIT_OBJECT_0+1 )
      { MSG msg;
	
	while( PeekMessage(&msg, NULL, 0, 0, PM_REMOVE) )
	{ TranslateMessage(&msg);
	  DispatchMessage(&msg);
	}
	
	if ( PL_handle_signals() < 0 )
	{ CloseHandle(h);
	  return FALSE;
	}
      } else
	break;
    }
    CloseHandle(h);

    return TRUE;
  } else				/* Pre NT implementation */
  { DWORD msecs = (DWORD)(t * 1000.0);

    while( msecs >= 100 )
    { Sleep(100);
      if ( PL_handle_signals() < 0 )
	return FALSE;
      msecs -= 100;
    }
    if ( msecs > 0 )
      Sleep(msecs);
    
    return TRUE;
  }
}

		 /*******************************
		 *	 QUERY CPU TIME		*
		 *******************************/

#define nano * 0.0000001
#define ntick 1.0			/* manual says 100.0 ??? */

double
CpuTime(cputime_kind which)
{ double t;
  HANDLE proc = GetCurrentProcess();
  FILETIME created, exited, kerneltime, usertime;

  if ( GetProcessTimes(proc, &created, &exited, &kerneltime, &usertime) )
  { FILETIME *p;

    switch ( which )
    { case CPU_USER:
	p = &usertime;
	break;
      case CPU_SYSTEM:
	p = &kerneltime;
        break;
    }
    t = (double)p->dwHighDateTime * (4294967296.0 * ntick nano);
    t += (double)p->dwLowDateTime  * (ntick nano);
  } else				/* '95, Windows 3.1/win32s */
  { extern long clock_wait_ticks;

    t = (double) (clock() - clock_wait_ticks) / (double) CLOCKS_PER_SEC;
  }

  return t;
}


char *
findExecutable(const char *module, char *exe)
{ int n;
  wchar_t wbuf[MAXPATHLEN];
  HMODULE hmod;

  if ( module )
  { if ( !(hmod = GetModuleHandle(module)) )
    { hmod = GetModuleHandle("libpl.dll");
      DEBUG(0,
	    Sdprintf("Warning: could not find module from \"%s\"\n"
		     "Warning: Trying %s to find home\n",
		     module,
		     hmod ? "\"LIBPL.DLL\"" : "executable"));
    }
  } else
    hmod = NULL;

  if ( (n = GetModuleFileNameW(hmod, wbuf, MAXPATHLEN)) > 0 )
  { char buf2[MAXPATHLEN];

    wbuf[n] = EOS;
    return _xos_long_file_name_toA(wbuf, buf2, MAXPATHLEN);
  } else if ( module )
  { char buf[MAXPATHLEN];
    PrologPath(module, buf, sizeof(buf));

    strcpy(exe, buf);
  } else
    *exe = EOS;

  return exe;
}

		 /*******************************
		 *     SUPPORT FOR SHELL/2	*
		 *******************************/

typedef struct
{ const char *name;
  int         id;
} showtype;

static int
get_showCmd(term_t show, int *cmd)
{ char *s;
  showtype *st;
  static showtype types[] =
  { { "hide", 		 SW_HIDE },
    { "maximize", 	 SW_MAXIMIZE },
    { "minimize", 	 SW_MINIMIZE },
    { "restore", 	 SW_RESTORE },
    { "show", 		 SW_SHOW },
    { "showdefault", 	 SW_SHOWDEFAULT },
    { "showmaximized",   SW_SHOWMAXIMIZED },
    { "showminimized",   SW_SHOWMINIMIZED },
    { "showminnoactive", SW_SHOWMINNOACTIVE },
    { "showna",          SW_SHOWNA },
    { "shownoactive",    SW_SHOWNOACTIVATE },
    { "shownormal",      SW_SHOWNORMAL },
					/* compatibility */
    { "normal", 	 SW_SHOWNORMAL },
    { "iconic", 	 SW_MINIMIZE },
    { NULL, 0 },
  };

  if ( show == 0 )
  { *cmd = SW_SHOWNORMAL;
    succeed;
  }

  if ( !PL_get_chars_ex(show, &s, CVT_ATOM) )
    fail;
  for(st=types; st->name; st++)
  { if ( streq(st->name, s) )
    { *cmd = st->id;
      succeed;
    }
  }

  return PL_error(NULL, 0, NULL, ERR_DOMAIN,
		  PL_new_atom("win_show"), show);
}



static int
win_exec(size_t len, const wchar_t *cmd, UINT show)
{ STARTUPINFOW startup;
  PROCESS_INFORMATION info;
  int rval;
  wchar_t *wcmd;

  memset(&startup, 0, sizeof(startup));
  startup.cb = sizeof(startup);
  startup.wShowWindow = show;

					/* ensure 0-terminated */
  wcmd = PL_malloc((len+1)*sizeof(wchar_t));
  memcpy(wcmd, cmd, len*sizeof(wchar_t));
  wcmd[len] = 0;
  
  rval = CreateProcessW(NULL,		/* app */
			wcmd,
			NULL, NULL,	/* security */
			FALSE,		/* inherit handles */
			0,		/* flags */
			NULL,		/* environment */
			NULL,		/* Directory */
			&startup,
			&info);		/* process info */
  PL_free(wcmd);

  if ( rval )
  { CloseHandle(info.hProcess);
    CloseHandle(info.hThread);

    succeed;
  } else
  { term_t tmp = PL_new_term_ref();
      
    PL_unify_wchars(tmp, PL_ATOM, len, cmd);
    return PL_error(NULL, 0, WinError(), ERR_SHELL_FAILED, tmp);
  }
}


static void
utf8towcs(wchar_t *o, const char *src)
{ for( ; *src; )
  { int wc;

    src = utf8_get_char(src, &wc);
    *o++ = wc;
  }
  *o = 0;
}


int
System(char *command)			/* command is a UTF-8 string */
{ STARTUPINFOW sinfo;
  PROCESS_INFORMATION pinfo;
  int shell_rval;
  int len;
  wchar_t *wcmd;

  memset(&sinfo, 0, sizeof(sinfo));
  sinfo.cb = sizeof(sinfo);

  len = utf8_strlen(command, strlen(command));
  wcmd = PL_malloc((len+1)*sizeof(wchar_t));
  utf8towcs(wcmd, command);

  if ( CreateProcessW(NULL,			/* module */
		      wcmd,			/* command line */
		      NULL,			/* Security stuff */
		      NULL,			/* Thread security stuff */
		      FALSE,			/* Inherit handles */
		      NORMAL_PRIORITY_CLASS,	/* flags */
		      NULL,			/* environment */
		      NULL,			/* CWD */
		      &sinfo,			/* startup info */
		      &pinfo) )			/* process into */
  { BOOL rval;
    DWORD code;

    CloseHandle(pinfo.hThread);			/* don't need this */
    PL_free(wcmd);

    do
    { MSG msg;

      if ( PeekMessage(&msg, NULL, 0, 0, PM_REMOVE) )
      { TranslateMessage(&msg);
	DispatchMessage(&msg);
      } else
	Sleep(50);

      rval = GetExitCodeProcess(pinfo.hProcess, &code);
    } while(rval == TRUE && code == STILL_ACTIVE);

    shell_rval = (rval == TRUE ? code : -1);
    CloseHandle(pinfo.hProcess);
  } else
  { PL_free(wcmd);
    return shell_rval = -1;
  }

  return shell_rval;
}


word
pl_win_exec(term_t cmd, term_t how)
{ wchar_t *s;
  size_t len;
  UINT h;

  if ( PL_get_wchars(cmd, &len, &s, CVT_ALL|CVT_EXCEPTION) &&
       get_showCmd(how, &h) )
  { return win_exec(len, s, h);
  } else
    fail;
}

typedef struct
{ int   eno;
  const char *message;
} shell_error;

static const shell_error se_errors[] = 
{ { 0 ,                     "Out of memory or resources" },
  { ERROR_FILE_NOT_FOUND,   "File not found" },
  { ERROR_PATH_NOT_FOUND,   "path not found" },
  { ERROR_BAD_FORMAT,	    "Invalid .EXE" },
  { SE_ERR_ACCESSDENIED,    "Access denied" },
  { SE_ERR_ASSOCINCOMPLETE, "Incomplete association" },
  { SE_ERR_DDEBUSY,	    "DDE server busy" },
  { SE_ERR_DDEFAIL,         "DDE transaction failed" },
  { SE_ERR_DDETIMEOUT,	    "DDE request timed out" },
  { SE_ERR_DLLNOTFOUND,	    "DLL not found" },
  { SE_ERR_FNF,		    "File not found (FNF)" },
  { SE_ERR_NOASSOC, 	    "No association" },
  { SE_ERR_OOM,		    "Not enough memory" }, 
  { SE_ERR_PNF,		    "Path not found (PNF)" },
  { SE_ERR_SHARE,	    "Sharing violation" },
  { 0,			    NULL }
};
 

static int
win_shell(term_t op, term_t file, term_t how)
{ unsigned int lo, lf;
  wchar_t *o, *f;
  UINT h;
  HINSTANCE instance;

  if ( !PL_get_wchars(op,   &lo, &o, CVT_ALL|CVT_EXCEPTION|BUF_RING) ||
       !PL_get_wchars(file, &lf, &f, CVT_ALL|CVT_EXCEPTION|BUF_RING) ||
       !get_showCmd(how, &h) )
    fail;
       
  instance = ShellExecuteW(NULL, o, f, NULL, NULL, h);

  if ( (long)instance <= 32 )
  { const shell_error *se;

    for(se = se_errors; se->message; se++)
    { if ( se->eno == (int)instance )
	return PL_error(NULL, 0, se->message, ERR_SHELL_FAILED, file);
    }
    PL_error(NULL, 0, NULL, ERR_SHELL_FAILED, file);
  }

  succeed;
}


static
PRED_IMPL("win_shell", 2, win_shell2, 0)
{ return win_shell(A1, A2, 0);
}


static
PRED_IMPL("win_shell", 3, win_shell3, 0)
{ return win_shell(A1, A2, A3);
}


char *
getenv3(const char *name, char *buf, unsigned int len)
{ if ( GetEnvironmentVariable(name, buf, (DWORD)len) )
    return buf;
  
  return NULL;
}

/* What does this return if the variable is not defined?
*/

int
getenvl(const char *name)
{ return GetEnvironmentVariable(name, NULL, 0);
}

#if _DEBUG
void
initHeapDebug(void)
{ int tmpFlag = _CrtSetDbgFlag( _CRTDBG_REPORT_FLAG );

  if ( !(tmpFlag & _CRTDBG_CHECK_ALWAYS_DF) )
  { /*PlMessage("Setting malloc() debugging");*/
    tmpFlag |= _CRTDBG_CHECK_ALWAYS_DF;
    _CrtSetDbgFlag(tmpFlag);
  } /*else
    PlMessage("Malloc debugging already set");*/
}
#endif

foreign_t
pl_win_module_file(term_t module, term_t file)
{ char buf[MAXPATHLEN];
  char *m;
  char *f;

  if ( !PL_get_chars_ex(module, &m, CVT_ALL) )
    fail;
  if ( (f = findExecutable(m, buf)) )
    return PL_unify_atom_chars(file, f);

  fail;
}

		 /*******************************
		 *	  WINDOWS MESSAGES	*
		 *******************************/

int
PL_win_message_proc(HWND hwnd, UINT message, UINT wParam, LONG lParam)
{
#ifdef O_PLMT
  if ( hwnd == NULL &&
       message == WM_SIGNALLED &&
       wParam == 0 &&			/* or another constant? */
       lParam == 0 )
  { if ( PL_handle_signals() < 0 )
      return PL_MSG_EXCEPTION_RAISED;

    return PL_MSG_HANDLED;
  }
#endif

  return PL_MSG_IGNORED;
}


		 /*******************************
		 *	DLOPEN AND FRIENDS	*
		 *******************************/

#ifdef EMULATE_DLOPEN

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
These functions emulate the bits from the ELF shared object interface we
need. They are used  by  pl-load.c,   which  defines  the  actual Prolog
interface.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static char *dlmsg;

void *
dlopen(const char *file, int flags)
{ HINSTANCE h;

  if ( (h = LoadLibrary(file)) )
  { dlmsg = "No Error";
    return (void *)h;
  }

  dlmsg = WinError();
  return NULL;
}


const char *
dlerror()
{ return dlmsg;
}


void *
dlsym(void *handle, char *symbol)
{ void *addr = GetProcAddress(handle, symbol);

  if ( addr )
  { dlmsg = "No Error";
    return addr;
  }
  
  dlmsg = WinError();
  return NULL;
}


int
dlclose(void *handle)
{ FreeLibrary(handle);

  return 0;
}

#endif /*EMULATE_DLOPEN*/


		 /*******************************
		 *	      REGISTRY		*
		 *******************************/

static HKEY
reg_open_key(const char *which, int create)
{ HKEY key = HKEY_CURRENT_USER;
  DWORD disp;
  LONG rval;

  while(*which)
  { char buf[256];
    char *s;
    HKEY tmp;

    for(s=buf; *which && !(*which == '/' || *which == '\\'); )
      *s++ = *which++;
    *s = '\0';
    if ( *which )
      which++;

    if ( streq(buf, "HKEY_CLASSES_ROOT") )
    { key = HKEY_CLASSES_ROOT;
      continue;
    } else if ( streq(buf, "HKEY_CURRENT_USER") )
    { key = HKEY_CURRENT_USER;
      continue;
    } else if ( streq(buf, "HKEY_LOCAL_MACHINE") )
    { key = HKEY_LOCAL_MACHINE;
      continue;
    } else if ( streq(buf, "HKEY_USERS") )
    { key = HKEY_USERS;
      continue;
    }

    DEBUG(2, Sdprintf("Trying %s\n", buf));
    if ( RegOpenKeyEx(key, buf, 0L, KEY_READ, &tmp) == ERROR_SUCCESS )
    { RegCloseKey(key);
      key = tmp;
      continue;
    }

    if ( !create )
      return NULL;

    rval = RegCreateKeyEx(key, buf, 0, "", 0,
			  KEY_ALL_ACCESS, NULL, &tmp, &disp);
    RegCloseKey(key);
    if ( rval == ERROR_SUCCESS )
      key = tmp;
    else
      return NULL;
  }

  return key;
}

#define MAXREGSTRLEN 1024

foreign_t
pl_get_registry_value(term_t Key, term_t Name, term_t Value)
{ DWORD type;
  BYTE  data[MAXREGSTRLEN];
  DWORD len = sizeof(data);
  char *k;
  char *name;
  HKEY key;

  if ( !PL_get_chars_ex(Key, &k, CVT_ATOM) ||
       !PL_get_chars_ex(Name, &name, CVT_ATOM) )
    return FALSE;
  if ( !(key=reg_open_key(k, FALSE)) )
    return PL_error(NULL, 0, NULL, ERR_EXISTENCE, PL_new_atom("key"), Key);

  DEBUG(9, Sdprintf("key = %p, name = %s\n", key, name));
  if ( RegQueryValueEx(key, name, NULL, &type, data, &len) == ERROR_SUCCESS )
  { RegCloseKey(key);

    switch(type)
    { case REG_SZ:
	return PL_unify_atom_chars(Value, data);
      case REG_DWORD:
	return PL_unify_integer(Value, *((DWORD *)data));
      default:
	return warning("get_registry_value/2: Unknown registery-type");
    }
  }

  return FALSE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Get the local, global,  trail  and   argument-stack  defaults  from  the
registry.  They  can  be  on  the   HKEY_CURRENT_USER  as  well  as  the
HKEY_LOCAL_MACHINE  registries  to  allow   for    both   user-only  and
system-wide settings.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static struct regdef
{ const char *name;
  int        *address;
} const regdefs[] =
{ { "localSize",    &GD->defaults.local },
  { "globalSize",   &GD->defaults.global },
  { "trailSize",    &GD->defaults.trail },
  { "argumentSize", &GD->defaults.argument },
  { NULL,           NULL }
};


static void
setStacksFromKey(HKEY key)
{ DWORD type;
  BYTE  data[128];
  DWORD len = sizeof(data);
  const struct regdef *rd;

  for(rd = regdefs; rd->name; rd++)
  { if ( RegQueryValueEx(key, rd->name, NULL, &type, data, &len) ==
							ERROR_SUCCESS &&
	 type == REG_DWORD )
    { DWORD v = *((DWORD *)data);
      
      *rd->address = (int)v;
    }
  }
}


void
getDefaultsFromRegistry()
{ HKEY key;

  if ( (key = reg_open_key("HKEY_LOCAL_MACHINE/Software/SWI/Prolog", FALSE)) )
  { setStacksFromKey(key);
    RegCloseKey(key);
  }
  if ( (key = reg_open_key("HKEY_CURRENT_USER/Software/SWI/Prolog", FALSE)) )
  { setStacksFromKey(key);
    RegCloseKey(key);
  }
}

		 /*******************************
		 *      PUBLISH PREDICATES	*
		 *******************************/

BeginPredDefs(win)
  PRED_DEF("win_shell", 2, win_shell2, 0)
  PRED_DEF("win_shell", 3, win_shell3, 0)
EndPredDefs

#endif /*__WINDOWS__*/

