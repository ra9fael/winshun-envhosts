#pragma once
#include <QGuiApplication>
#include <QIcon>

inline void setPluginWindowIcon()
{
    QIcon icon;
    icon.addFile(QStringLiteral(":/plugin-icons/app-16.png"), QSize(16, 16));
    icon.addFile(QStringLiteral(":/plugin-icons/app-20.png"), QSize(20, 20));
    icon.addFile(QStringLiteral(":/plugin-icons/app-24.png"), QSize(24, 24));
    icon.addFile(QStringLiteral(":/plugin-icons/app-32.png"), QSize(32, 32));
    icon.addFile(QStringLiteral(":/plugin-icons/app-40.png"), QSize(40, 40));
    icon.addFile(QStringLiteral(":/plugin-icons/app-48.png"), QSize(48, 48));
    icon.addFile(QStringLiteral(":/plugin-icons/app-64.png"), QSize(64, 64));
    icon.addFile(QStringLiteral(":/plugin-icons/app-256.png"), QSize(256, 256));
    QGuiApplication::setWindowIcon(icon);
}
