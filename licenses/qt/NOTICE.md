# Qt runtime

The QML package dynamically links the Qt libraries deployed with this package.
Qt is copyright The Qt Company Ltd. and contributors. License text is included
in LICENSE. The GDI package does not contain or require Qt.

The maintained release configuration uses Qt 6.8.3 (MSVC x64). Official sources,
including third-party notices, are available at:
https://download.qt.io/archive/qt/6.8/6.8.3/single/
https://code.qt.io/cgit/qt/qtbase.git/tree/?h=v6.8.3
https://code.qt.io/cgit/qt/qtdeclarative.git/tree/?h=v6.8.3

This plugin's source and build instructions are available at:
https://github.com/ra9fael/winshun-envhosts/tree/qml

The Qt DLLs are separate from envhosts.exe. They may be replaced with compatible
builds; this plugin imposes no restriction on modifying or reverse engineering
the application for debugging modifications to these LGPL libraries. A changed
plugin package may require confirmation again in WinShun.
