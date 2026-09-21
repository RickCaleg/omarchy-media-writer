/*
 * Fedora Media Writer
 * Copyright (C) 2024 Jan Grulich <jgrulich@redhat.com>
 * Copyright (C) 2016 Martin Bříza <mbriza@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "restorejob.h"

#include <QCoreApplication>
#include <QThread>
#include <QTimer>

#include <QDBusArgument>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>

#include <stdio.h>
#include <sys/types.h>

RestoreJob::RestoreJob(const QString &where)
    : Job(where)
{
    QTimer::singleShot(0, this, SLOT(work()));
}

void RestoreJob::work()
{
    out << "0\n";
    out.flush();

    QDBusInterface device("org.freedesktop.UDisks2", where, "org.freedesktop.UDisks2.Block", QDBusConnection::systemBus(), this);
    QString drivePath = qvariant_cast<QDBusObjectPath>(device.property("Drive")).path();
    QDBusInterface drive("org.freedesktop.UDisks2", drivePath, "org.freedesktop.UDisks2.Drive", QDBusConnection::systemBus(), this);
    QDBusInterface manager("org.freedesktop.UDisks2", "/org/freedesktop/UDisks2", "org.freedesktop.DBus.ObjectManager", QDBusConnection::systemBus());
    QDBusMessage message = manager.call("GetManagedObjects");

    if (message.arguments().length() == 1) {
        QDBusArgument arg = qvariant_cast<QDBusArgument>(message.arguments().first());
        DBusIntrospection objects;
        arg >> objects;
        for (auto i : objects.keys()) {
            if (objects[i].contains("org.freedesktop.UDisks2.Filesystem")) {
                QString currentDrivePath = qvariant_cast<QDBusObjectPath>(objects[i]["org.freedesktop.UDisks2.Block"]["Drive"]).path();
                if (currentDrivePath == drivePath) {
                    QDBusInterface partition("org.freedesktop.UDisks2", i.path(), "org.freedesktop.UDisks2.Filesystem", QDBusConnection::systemBus());
                    message = partition.call("Unmount", Properties{{"force", true}});
                }
            }
        }
    }

    out << "15\n";
    out.flush();

    // Wipe out first and last 128 blocks with zeroes
    fd = getDescriptor();
    if (fd.fileDescriptor() < 0) {
        err << tr("Failed to open device for writing");
        err.flush();
        qApp->exit(1);
        return;
    }

    auto bufferOwner = pageAlignedBuffer();
    char *buffer = std::get<1>(bufferOwner);
    qint64 size = std::get<2>(bufferOwner);

    memset(buffer, '\0', size);

    off_t filesize = lseek(fd.fileDescriptor(), 0, SEEK_END);
    if (filesize == static_cast<off_t>(-1) || lseek(fd.fileDescriptor(), 0, SEEK_SET) == static_cast<off_t>(-1)) {
        err << tr("Failed to get file size");
        err.flush();
        qApp->exit(1);
        return;
    }

    // Zero the first and last 128 blocks (512 MiB each), where partition
    // tables, the ISO9660 volume descriptors and the backup GPT live. On a
    // drive too small for that, zero each half instead of running off its end.
    const off_t wipe = qMin<off_t>(128 * size, (filesize / 2) / 4096 * 4096);
    auto zero = [&](off_t from) {
        if (lseek(fd.fileDescriptor(), from, SEEK_SET) == static_cast<off_t>(-1))
            return false;
        for (off_t done = 0; done < wipe;) {
            const qint64 chunk = qMin<off_t>(size, wipe - done);
            if (::write(fd.fileDescriptor(), buffer, chunk) != chunk)
                return false;
            done += chunk;
        }
        return true;
    };

    if (!zero(0)) {
        err << tr("Destination drive is not writable");
        err.flush();
        qApp->exit(1);
        return;
    }

    out << "35\n";
    out.flush();

    if (!zero((filesize - wipe) / 4096 * 4096)) {
        err << tr("Destination drive is not writable");
        err.flush();
        qApp->exit(1);
        return;
    }

    // Ensure data is flushed to disk
    if (::fsync(fd.fileDescriptor()) == -1) {
        err << tr("Failed to sync data to disk");
        err.flush();
        qApp->exit(1);
        return;
    }

    out << "55\n";
    out.flush();

    // Close the file descriptor before handing off to UDisks2 to avoid conflicts
    fd = QDBusUnixFileDescriptor();

    // Formatting a large drive, or authorizing it, can outlast the default timeout.
    device.setTimeout(DBUS_AUTH_TIMEOUT);
    QDBusReply<void> formatReply = device.call("Format", "gpt", Properties());
    if (!formatReply.isValid() && formatReply.error().type() != QDBusError::NoReply) {
        err << formatReply.error().message() << "\n";
        err.flush();
        qApp->exit(1);
        return;
    }

    out << "75\n";
    out.flush();

    QDBusInterface partitionTable("org.freedesktop.UDisks2", where, "org.freedesktop.UDisks2.PartitionTable", QDBusConnection::systemBus(), this);
    partitionTable.setTimeout(DBUS_AUTH_TIMEOUT);
    QDBusReply<QDBusObjectPath> partitionReply = partitionTable.call("CreatePartition", 1ULL, 0ULL, "", "", Properties());
    if (!partitionReply.isValid()) {
        err << partitionReply.error().message();
        err.flush();
        qApp->exit(2);
        return;
    }
    QString partitionPath = partitionReply.value().path();
    QDBusInterface partition("org.freedesktop.UDisks2", partitionPath, "org.freedesktop.UDisks2.Block", QDBusConnection::systemBus(), this);
    partition.setTimeout(DBUS_AUTH_TIMEOUT);
    QDBusReply<void> formatPartitionReply = partition.call("Format", "exfat", Properties{{"update-partition-type", true}});
    if (!formatPartitionReply.isValid() && formatPartitionReply.error().type() != QDBusError::NoReply) {
        err << formatPartitionReply.error().message() << "\n";
        err.flush();
        qApp->exit(3);
        return;
    }

    out << "100\n";
    out.flush();
    err.flush();

    qApp->exit(0);
}
