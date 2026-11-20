# Privacy

House of the Dead VR (every platform: Meta Quest, Steam Frame, Windows and PC VR, Mac, Android, iPhone and iPad,
Apple TV, Apple Vision Pro) has no accounts, no ads and no analytics, and it doesn't track you.

**On your device.** Your ROM set, your settings, the game's saves and high scores, and a log of the app's own messages
stay in the app's own folder. Nothing in it is sent anywhere unless described below.

**Online play (only when you use it).** Hosting or joining an online game connects to a lobby server (zonesync.net
by default; you can change it in the ONLINE tab) or straight to the other player on your local network. What is sent
is the game's inputs, your player name (PLAYER unless you change it), the room's code and, with voice chat on, your
voice. The lobby server sees your IP address, as any internet service does. Voice chat uses your microphone only while
voice chat is on, and only during an online game; the voice goes to the other player and is not recorded.

**Crash reports (on unless you switch them off).** If the app crashes, it sends a report the next time it starts, on
every platform. The report holds: where in the app it crashed (the kind of fault and the addresses in the app's code),
the app's version and build, the last lines of the app's own log, the app's settings, the device's model, its system
version and number of processor cores, and the time of the crash. No name, account or ROM data is in it, and no file
paths outside the app's folder; IP and e-mail addresses that appear in the log are removed. It goes to this project's
server (hotdvr.roccojuliano.com), which files it in a private GitHub repository that only the developer can read. The
server sees your IP address, as any internet service does, and uses it only to limit how many reports one address
sends; it doesn't keep it. The SYSTEM tab's Crash reports switch turns this off; then nothing is sent.

**Updates (Quest and Steam Frame).** The app asks GitHub for this project's list of releases when it starts, and
downloads an update only when you choose to. GitHub sees that request as it sees any visit.

Questions: open an issue at https://github.com/MrSco/house-of-the-dead-vr/issues.
