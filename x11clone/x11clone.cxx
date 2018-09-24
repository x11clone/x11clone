/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2011 Pierre Ossman <ossman@cendio.se> for Cendio AB
 * Copyright (C) 2011 D. R. Commander.  All Rights Reserved.
 * Copyright 2018 Peter Astrand <astrand@cendio.se> for Cendio AB
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <locale.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef WIN32
#include <os/winerrno.h>
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

#if !defined(WIN32) && !defined(__APPLE__)
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#endif

#include <rfb/Logger_stdio.h>
#include <rfb/SecurityClient.h>
#include <rfb/Security.h>
#include <rfb/LogWriter.h>
#include <rfb/Timer.h>
#include <rfb/Exception.h>
#include <network/TcpSocket.h>
#include <os/os.h>

#include <FL/Fl.H>
#include <FL/Fl_Widget.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Sys_Menu_Bar.H>
#include <FL/fl_ask.H>
#include <FL/x.H>

#include "i18n.h"
#include "parameters.h"
#include "CConn.h"
#include "ServerDialog.h"
#include "UserDialog.h"
#include "vncviewer.h"
#include "fltk_layout.h"

#ifdef WIN32
#include "resource.h"
#include "win32.h"
#endif

rfb::LogWriter vlog("main");

using namespace network;
using namespace rfb;
using namespace std;

char serverName[VNCSERVERNAMELEN] = { '\0' }; // "server display"

static const char *argv0 = NULL;

static bool exitMainloop = false;
static const char *exitError = NULL;

static const char *about_text()
{
  static char buffer[1024];

  // This is used in multiple places with potentially different
  // encodings, so we need to make sure we get a fresh string every
  // time.
  snprintf(buffer, sizeof(buffer),
           _("x11clone %d-bit v%s\n"
             "Built on: %s\n"
             "Copyright 2018 Peter Astrand for Cendio AB\n"
             "Copyright (C) 1999-%d TigerVNC Team and many others (see README.rst)\n"
             "See http://www.tigervnc.org for information on TigerVNC."),
           (int)sizeof(size_t)*8, PACKAGE_VERSION,
           BUILD_TIMESTAMP, 2018);

  return buffer;
}

void exit_vncviewer(const char *error)
{
  // Prioritise the first error we get as that is probably the most
  // relevant one.
  if ((error != NULL) && (exitError == NULL))
    exitError = strdup(error);

  exitMainloop = true;
}

bool should_exit()
{
  return exitMainloop;
}

void about_vncviewer()
{
  fl_message_title(_("About x11clone"));
  fl_message("%s", about_text());
}

void run_mainloop()
{
  int next_timer;

  next_timer = Timer::checkTimeouts();
  if (next_timer == 0)
    next_timer = INT_MAX;

  if (Fl::wait((double)next_timer / 1000.0) < 0.0) {
    vlog.error(_("Internal FLTK error. Exiting."));
    exit(-1);
  }
}

#ifdef __APPLE__
static void about_callback(Fl_Widget *widget, void *data)
{
  about_vncviewer();
}

static void new_connection_cb(Fl_Widget *widget, void *data)
{
  const char *argv[2];
  pid_t pid;

  pid = fork();
  if (pid == -1) {
    vlog.error(_("Error starting new x11clone: %s"), strerror(errno));
    return;
  }

  if (pid != 0)
    return;

  argv[0] = argv0;
  argv[1] = NULL;

  execvp(argv[0], (char * const *)argv);

  vlog.error(_("Error starting new x11clone: %s"), strerror(errno));
  _exit(1);
}
#endif

static void CleanupSignalHandler(int sig)
{
  // CleanupSignalHandler allows C++ object cleanup to happen because it calls
  // exit() rather than the default which is to abort.
  vlog.info(_("Termination signal %d has been received. x11clone will now exit."), sig);
  exit(1);
}

static void init_fltk()
{
  // Basic text size (10pt @ 96 dpi => 13px)
  FL_NORMAL_SIZE = 13;

  // Select a FLTK scheme and background color that looks somewhat
  // close to modern systems
  Fl::scheme("gtk+");
  Fl::background(220, 220, 220);

  // macOS has a slightly brighter default background though
#ifdef __APPLE__
  Fl::background(240, 240, 240);
#endif

  // Proper Gnome Shell integration requires that we set a sensible
  // WM_CLASS for the window.
  Fl_Window::default_xclass("x11clone");

  // Set the default icon for all windows.
#ifdef WIN32
  HICON lg, sm;

  lg = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON),
                        IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                        GetSystemMetrics(SM_CYICON),
                        LR_DEFAULTCOLOR | LR_SHARED);
  sm = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON),
                        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                        GetSystemMetrics(SM_CYSMICON),
                        LR_DEFAULTCOLOR | LR_SHARED);

  Fl_Window::default_icons(lg, sm);
#elif ! defined(__APPLE__)
  const int icon_sizes[] = {48, 32, 24, 16};

  Fl_PNG_Image *icons[4];
  int count;

  count = 0;

  // FIXME: Follow icon theme specification
  for (size_t i = 0;i < sizeof(icon_sizes)/sizeof(icon_sizes[0]);i++) {
      char icon_path[PATH_MAX];
      bool exists;

      sprintf(icon_path, "%s/icons/hicolor/%dx%d/apps/tigervnc.png",
              DATA_DIR, icon_sizes[i], icon_sizes[i]);

#ifndef WIN32
      struct stat st;
      if (stat(icon_path, &st) != 0)
#else
      struct _stat st;
      if (_stat(icon_path, &st) != 0)
          return(false);
#endif
        exists = false;
      else
        exists = true;

      if (exists) {
          icons[count] = new Fl_PNG_Image(icon_path);
          if (icons[count]->w() == 0 ||
              icons[count]->h() == 0 ||
              icons[count]->d() != 4) {
              delete icons[count];
              continue;
          }

          count++;
      }
  }

  Fl_Window::default_icons((const Fl_RGB_Image**)icons, count);

  for (int i = 0;i < count;i++)
      delete icons[i];
#endif

  // This makes the "icon" in dialogs rounded, which fits better
  // with the above schemes.
  fl_message_icon()->box(FL_UP_BOX);

  // Turn off the annoying behaviour where popups track the mouse.
  fl_message_hotspot(false);

  // Avoid empty titles for popups
  fl_message_title_default(_("x11clone"));

#ifdef WIN32
  // Most "normal" Windows apps use this font for UI elements.
  Fl::set_font(FL_HELVETICA, "Tahoma");
#endif

  // FLTK exposes these so that we can translate them.
  fl_no     = _("No");
  fl_yes    = _("Yes");
  fl_ok     = _("OK");
  fl_cancel = _("Cancel");
  fl_close  = _("Close");

#ifdef __APPLE__
  /* Needs trailing space */
  static char fltk_about[16];
  snprintf(fltk_about, sizeof(fltk_about), "%s ", _("About"));
  Fl_Mac_App_Menu::about = fltk_about;
  static char fltk_hide[16];
  snprintf(fltk_hide, sizeof(fltk_hide), "%s ", _("Hide"));
  Fl_Mac_App_Menu::hide = fltk_hide;
  static char fltk_quit[16];
  snprintf(fltk_quit, sizeof(fltk_quit), "%s ", _("Quit"));
  Fl_Mac_App_Menu::quit = fltk_quit;

  Fl_Mac_App_Menu::print = ""; // Don't want the print item
  Fl_Mac_App_Menu::services = _("Services");
  Fl_Mac_App_Menu::hide_others = _("Hide Others");
  Fl_Mac_App_Menu::show = _("Show All");

  fl_mac_set_about(about_callback, NULL);

  Fl_Sys_Menu_Bar *menubar;
  char buffer[1024];
  menubar = new Fl_Sys_Menu_Bar(0, 0, 500, 25);
  // Fl_Sys_Menu_Bar overrides methods without them being virtual,
  // which means we cannot use our generic Fl_Menu_ helpers.
  if (fltk_menu_escape(p_("SysMenu|", "&File"),
                       buffer, sizeof(buffer)) < sizeof(buffer))
      menubar->add(buffer, 0, 0, 0, FL_SUBMENU);
  if (fltk_menu_escape(p_("SysMenu|File|", "&New Connection"),
                       buffer, sizeof(buffer)) < sizeof(buffer))
      menubar->insert(1, buffer, FL_COMMAND | 'n', new_connection_cb);
#endif
}

static void mkvnchomedir()
{
  // Create .vnc in the user's home directory if it doesn't already exist
  char* homeDir = NULL;

  if (getvnchomedir(&homeDir) == -1) {
    vlog.error(_("Could not create VNC home directory: can't obtain home "
                 "directory path."));
  } else {
    int result = mkdir(homeDir, 0755);
    if (result == -1 && errno != EEXIST)
      vlog.error(_("Could not create VNC home directory: %s."), strerror(errno));
    delete [] homeDir;
  }
}

static void setRemoveParam(const char* param, const char* value)
{
    if (value) {
	Configuration::setParam(param, value);
    }
    Configuration::removeParam(param);
}

static void usage(const char *programName)
{
#ifdef WIN32
  // If we don't have a console then we need to create one for output
  if (GetConsoleWindow() == NULL) {
    HANDLE handle;
    int fd;

    AllocConsole();

    handle = GetStdHandle(STD_ERROR_HANDLE);
    fd = _open_osfhandle((intptr_t)handle, O_TEXT);
    *stderr = *fdopen(fd, "w");
  }
#endif

  fprintf(stderr,
          "\nusage: %s [parameters] [serverDisplay] [parameters]\n"
          "       %s [parameters] [.tigervnc file]\n",
          programName, programName);
  fprintf(stderr,"\n"
          "Parameters can be turned on with -<param> or off with -<param>=0\n"
          "Parameters which take a value can be specified as "
          "-<param> <value>\n"
          "Other valid forms are <param>=<value> -<param>=<value> "
          "--<param>=<value>\n"
          "Parameter names are case-insensitive.  The parameters are:\n\n");
  Configuration::listParams(79, 14);

#ifdef WIN32
  // Just wait for the user to kill the console window
  Sleep(INFINITE);
#endif

  exit(1);
}

static void
potentiallyLoadConfigurationFile(char *serverName)
{
  const bool hasPathSeparator = (strchr(serverName, '/') != NULL ||
                                 (strchr(serverName, '\\')) != NULL);

  if (hasPathSeparator) {
#ifndef WIN32
    struct stat sb;

    // This might be a UNIX socket, we need to check
    if (stat(serverName, &sb) == -1) {
      // Some access problem; let loadViewerParameters() deal with it...
    } else {
      if ((sb.st_mode & S_IFMT) == S_IFSOCK)
        return;
    }
#endif

    try {
      const char* newServerName;
      newServerName = loadViewerParameters(serverName);
      // This might be empty, but we still need to clear it so we
      // don't try to connect to the filename
      strncpy(serverName, newServerName, VNCSERVERNAMELEN);
    } catch (rfb::Exception& e) {
      vlog.error("%s", e.str());
      if (alertOnFatalError)
        fl_alert("%s", e.str());
      exit(EXIT_FAILURE);
    }
  }
}

#ifndef WIN32
static int
interpretViaParam(int *remotePort, int localPort)
{
  const int SERVER_PORT_OFFSET = 5900;
  char *pos = strchr(serverName, ':');
  if (pos == NULL)
    *remotePort = SERVER_PORT_OFFSET;
  else {
    int portOffset = SERVER_PORT_OFFSET;
    size_t len;
    *pos++ = '\0';
    len = strlen(pos);
    if (*pos == ':') {
      /* Two colons is an absolute port number, not an offset. */
      pos++;
      len--;
      portOffset = 0;
    }
    if (!len || strspn (pos, "-0123456789") != len )
      return 1;
    *remotePort = atoi(pos) + portOffset;
  }

  snprintf(serverName, VNCSERVERNAMELEN, "localhost::%d", localPort);
  serverName[VNCSERVERNAMELEN - 1] = '\0';

  return 0;
}

static void
createTunnel(const char *gatewayHost,
             int remotePort, int localPort)
{
  const char *cmd = getenv("VNC_VIA_CMD");
  char *cmd2, *percent;
  char lport[10], rport[10];
  sprintf(lport, "%d", localPort);
  sprintf(rport, "%d", remotePort);
  setenv("G", gatewayHost, 1);
  setenv("R", rport, 1);
  setenv("L", lport, 1);
  if (!cmd)
    cmd = "ssh -f -L \"$L\":localhost:\"$R\" \"$G\" sleep 20";
  /* Compatibility with TigerVNC's method. */
  cmd2 = strdup(cmd);
  while ((percent = strchr(cmd2, '%')) != NULL)
    *percent = '$';
  system(cmd2);
  free(cmd2);
}

static int mktunnel()
{
  const char *gatewayHost;
  int localPort = findFreeTcpPort();
  int remotePort;

  gatewayHost = strDup(via.getValueStr());
  if (interpretViaParam(&remotePort, localPort) != 0)
    return 1;

  createTunnel(gatewayHost, remotePort, localPort);

  return 0;
}
#endif /* !WIN32 */

int main(int argc, char** argv)
{
  UserDialog dlg;

  argv0 = argv[0];

  setlocale(LC_ALL, "");
  bindtextdomain(PACKAGE_NAME, LOCALE_DIR);
  textdomain(PACKAGE_NAME);

  rfb::SecurityClient::setDefaults();

  // Write about text to console, still using normal locale codeset
  fprintf(stderr,"\n%s\n", about_text());

  // Set gettext codeset to what our GUI toolkit uses. Since we are
  // passing strings from strerror/gai_strerror to the GUI, these must
  // be in GUI codeset as well.
  bind_textdomain_codeset(PACKAGE_NAME, "UTF-8");
  bind_textdomain_codeset("libc", "UTF-8");

  rfb::initStdIOLoggers();
#ifdef WIN32
  rfb::initFileLogger("C:\\temp\\x11clone.log");
#else
  rfb::initFileLogger("/tmp/x11clone.log");
#endif
  rfb::LogWriter::setLogParams("*:stderr:30");

#ifdef SIGHUP
  signal(SIGHUP, CleanupSignalHandler);
#endif
  signal(SIGINT, CleanupSignalHandler);
  signal(SIGTERM, CleanupSignalHandler);

  init_fltk();

  setRemoveParam("UseIPv6", NULL);
  setRemoveParam("UseIPv4", NULL);
  setRemoveParam("Shared", NULL);
  setRemoveParam("PasswordFile", NULL);
  setRemoveParam("passwd", NULL);
  setRemoveParam("listen", NULL);
  // Change default for RemoteResize
  Configuration::setParam("RemoteResize", "0");

  /* Load the default parameter settings */
  char defaultServerName[VNCSERVERNAMELEN] = "";
  try {
    const char* configServerName;
    configServerName = loadViewerParameters(NULL);
    if (configServerName != NULL)
      strncpy(defaultServerName, configServerName, VNCSERVERNAMELEN);
  } catch (rfb::Exception& e) {
    vlog.error("%s", e.str());
    if (alertOnFatalError)
      fl_alert("%s", e.str());
  }

  for (int i = 1; i < argc;) {
    if (Configuration::setParam(argv[i])) {
      i++;
      continue;
    }

    if (argv[i][0] == '-') {
      if (i+1 < argc) {
        if (Configuration::setParam(&argv[i][1], argv[i+1])) {
          i += 2;
          continue;
        }
      }

      usage(argv[0]);
    }

    strncpy(serverName, argv[i], VNCSERVERNAMELEN);
    serverName[VNCSERVERNAMELEN - 1] = '\0';
    i++;
  }

  // Check if the server name in reality is a configuration file
  potentiallyLoadConfigurationFile(serverName);

  mkvnchomedir();

#if !defined(WIN32) && !defined(__APPLE__)
  if (strcmp(display, "") != 0) {
    Fl::display(display);
  }
  fl_open_display();
  XkbSetDetectableAutoRepeat(fl_display, True, NULL);
#endif

  CSecurity::upg = &dlg;

  Socket *sock = NULL;

  {
    if (serverName[0] == '\0') {
      ServerDialog::run(defaultServerName, serverName);
      if (serverName[0] == '\0')
        return 1;
    }

#ifndef WIN32
    if (strlen (via.getValueStr()) > 0 && mktunnel() != 0)
      usage(argv[0]);
#endif
  }

  CConn *cc = new CConn(serverName, sock);

  while (!exitMainloop)
    run_mainloop();

  delete cc;

  if (exitError != NULL && alertOnFatalError)
    fl_alert("%s", exitError);

  return 0;
}
