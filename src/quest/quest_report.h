// Crash reports (the SYSTEM tab's Crash reports, on by default): after a crash, the next start turns crash.txt and the
// end of the crashed run's port.log into a JSON report and posts it, from a background thread, to the crash relay
// (tools/vr/crash-relay: it files or updates a GitHub issue). See quest_report.c. Every Android build: the Quest, the
// Steam Frame, the phones and tablets.
#pragma once
#include <android/native_activity.h>

// Crashes caught from now on (a signal handler appending to <dir>/crash.txt): as soon as the files folder exists.
void qreport_handler(const char *dir);
// The GNU build id of libhotdvr.so (40 hex digits; "" if it has none): the key of its symbols (quest_tools.py symbols).
const char *qreport_build_id(void);
// At start, after env.txt and the settings are read and before the game thread runs (it starts a new port.log):
// with reports on and a relay address set, a crash in crash.txt becomes crash-reports/<time>.json (crash.txt is
// emptied), and every report not sent yet is posted in the background. opt: the settings (quest_options.h).
void qreport_start(ANativeActivity *act, const char *files, const int *opt);
