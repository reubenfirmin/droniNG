/*
 *   Copyright 2010-2011, Christian Muehlhaeuser <muesli@tomahawk-player.org>
 *   Copyright 2014,      Dominik Schmidt <domme@tomahawk-player.org>
 *
 *   libcrashreporter is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Tomahawk is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Tomahawk. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Handler.h"

#include <string>
#include <iostream>

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>

#include <QDebug>

#ifdef __APPLE__
#   include <client/mac/handler/exception_handler.h>
#elif defined __linux__
#   include <client/linux/handler/exception_handler.h>
#   include <client/linux/handler/minidump_descriptor.h>
#endif

namespace CrashReporter
{

bool s_active = true;


void
Handler::setActive( bool enabled )
{
    s_active = enabled;
}


bool
Handler::isActive()
{
    return s_active;
}

#include <unistd.h>


static bool
#ifdef Q_OS_LINUX
LaunchUploader( const google_breakpad::MinidumpDescriptor& descriptor, void* context, bool succeeded )
{
#else // Q_OS_MAC
LaunchUploader( const char* dump_dir, const char* minidump_id, void* context, bool succeeded)
{
#endif
        if ( !succeeded )
        {
            printf("Could not write crash dump file");
            return false;
        }

        // DON'T USE THE HEAP!!!
        // So that indeed means, no QStrings, no qDebug(), no QAnything, seriously!

    #ifdef Q_OS_LINUX
        const char* path = descriptor.path();
    #else // Q_OS_MAC
        const char* extension = "dmp";

        char path[4096];
        strcpy(path, dump_dir);
        strcat(path, "/");
        strcat(path, minidump_id);
        strcat(path, ".");
        strcat(path,  extension);
    #endif

        printf("Dump file was written to: %s\n", path);

        const char* crashReporter = static_cast<Handler*>(context)->crashReporterChar();
        if ( !s_active || strlen( crashReporter ) == 0 )
            return false;

        pid_t pid = fork();
        if ( pid == -1 ) // fork failed
            return false;
        if ( pid == 0 )
        {
            // we are the fork
            execl( crashReporter,
                   crashReporter,
                   path,
                   (char*) 0 );

            // execl replaces this process, so no more code will be executed
            // unless it failed. If it failed, then we should return false.
            printf( "Error: Can't launch CrashReporter!\n" );
            return false;
        }

        // we called fork()
        return true;
    }


Handler::Handler( const QString& dumpFolderPath, bool active, const QString& crashReporter  )
{
    s_active = active;

    #if defined Q_OS_LINUX
    m_crash_handler =  new google_breakpad::ExceptionHandler( google_breakpad::MinidumpDescriptor(dumpFolderPath.toStdString()), NULL, LaunchUploader, this, true, -1 );
    #elif defined Q_OS_MAC
    m_crash_handler =  new google_breakpad::ExceptionHandler( dumpFolderPath.toStdString(), NULL, LaunchUploader, this, true, NULL);
    #endif

    setCrashReporter( crashReporter );
}


void
Handler::setCrashReporter( const QString& crashReporter )
{
    QString crashReporterPath;
    QString localReporter = QString( "%1/%2" ).arg( QCoreApplication::instance()->applicationDirPath() ).arg( crashReporter );
    QString globalReporter = QString( "%1/../libexec/%2" ).arg( QCoreApplication::instance()->applicationDirPath() ).arg( crashReporter );

    if ( QFileInfo( localReporter ).exists() )
        crashReporterPath = localReporter;
    else if ( QFileInfo( globalReporter ).exists() )
        crashReporterPath = globalReporter;
    else {
        qDebug() << "Could not find \"" << crashReporter << "\" in ../libexec or application path";
        crashReporterPath = crashReporter;
    }


    // cache reporter path as char*
    char* creporter;
    std::string sreporter = crashReporterPath.toStdString();
    creporter = new char[ sreporter.size() + 1 ];
    strcpy( creporter, sreporter.c_str() );
    m_crashReporterChar = creporter;

}


Handler::~Handler()
{
    delete m_crash_handler;
}

} // end namespace
