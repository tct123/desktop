/*
 * Copyright (C) 2022 by Claudio Cambra <claudio.cambra@nextcloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "shareemodel.h"

#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>

#include "ocsshareejob.h"

namespace OCC {

Q_LOGGING_CATEGORY(lcShareeModel, "com.nextcloud.shareemodel")

ShareeModel::ShareeModel(QObject *parent)
    : QAbstractListModel(parent)
{
    _userStoppedTypingTimer.setSingleShot(true);
    _userStoppedTypingTimer.setInterval(500);
    connect(&_userStoppedTypingTimer, &QTimer::timeout, this, &ShareeModel::fetch);
}

// ---------------------- QAbstractListModel methods ---------------------- //

int ShareeModel::rowCount(const QModelIndex &parent) const
{
    if(parent.isValid() || !_accountState) {
        return 0;
    }

    return _sharees.count();
}

QHash<int, QByteArray> ShareeModel::roleNames() const
{
    auto roles = QAbstractListModel::roleNames();
    roles[ShareeRole] = "sharee";
    roles[AutoCompleterStringMatchRole] = "autoCompleterStringMatch";

    return roles;
}

QVariant ShareeModel::data(const QModelIndex &index, const int role) const
{
    if (index.row() < 0 || index.row() > _sharees.size()) {
        return {};
    }

    const auto sharee = _sharees.at(index.row());

    if(sharee.isNull()) {
        return {};
    }

    switch(role) {
    case Qt::DisplayRole:
        return sharee->format();
    case AutoCompleterStringMatchRole:
        // Don't show this to the user
        return QString(sharee->displayName() + " (" + sharee->shareWith() + ")");
    case ShareeRole:
        return QVariant::fromValue(sharee);
    }

    qCWarning(lcShareeModel) << "Got unknown role" << role << "returning null value.";
    return {};
}

// --------------------------- QPROPERTY methods --------------------------- //

AccountState *ShareeModel::accountState() const
{
    return _accountState;
}

void ShareeModel::setAccountState(AccountState *accountState)
{
    if (accountState == _accountState) {
        return;
    }

    _accountState = accountState;
    Q_EMIT accountStateChanged();
}

bool ShareeModel::shareItemIsFolder() const
{
    return _shareItemIsFolder;
}

void ShareeModel::setShareItemIsFolder(const bool shareItemIsFolder)
{
    if (shareItemIsFolder == _shareItemIsFolder) {
        return;
    }

    _shareItemIsFolder = shareItemIsFolder;
    Q_EMIT shareItemIsFolderChanged();
}

QString ShareeModel::searchString() const
{
    return _searchString;
}

void ShareeModel::setSearchString(const QString &searchString)
{
    if (searchString == _searchString) {
        return;
    }

    _searchString = searchString;
    Q_EMIT searchStringChanged();

    _userStoppedTypingTimer.start();
}

bool ShareeModel::fetchOngoing() const
{
    return _fetchOngoing;
}

ShareeModel::LookupMode ShareeModel::lookupMode() const
{
    return _lookupMode;
}

void ShareeModel::setLookupMode(const ShareeModel::LookupMode lookupMode)
{
    if (lookupMode == _lookupMode) {
        return;
    }

    _lookupMode = lookupMode;
    Q_EMIT lookupModeChanged();
}

// ------------------------- Internal data methods ------------------------- //

void ShareeModel::fetch()
{
    if(!_accountState || !_accountState->account() || _searchString.isEmpty()) {
        qCInfo(lcShareeModel) << "Not fetching sharees for searchString: " << _searchString;
        return;
    }

    _fetchOngoing = true;
    Q_EMIT fetchOngoingChanged();

    const auto shareItemTypeString = _shareItemIsFolder ? QStringLiteral("folder") : QStringLiteral("file");

    auto *job = new OcsShareeJob(_accountState->account());

    connect(job, &OcsShareeJob::shareeJobFinished, this, &ShareeModel::shareesFetched);
    connect(job, &OcsJob::ocsError, this, [&](const int statusCode, const QString &message) {
        _fetchOngoing = false;
        Q_EMIT fetchOngoingChanged();
        Q_EMIT ShareeModel::displayErrorMessage(statusCode, message);
    });

    job->getSharees(_searchString, shareItemTypeString, 1, 50, _lookupMode == LookupMode::GlobalSearch ? true : false);
}

void ShareeModel::shareesFetched(const QJsonDocument &reply)
{
    _fetchOngoing = false;
    Q_EMIT fetchOngoingChanged();

    qCInfo(lcShareeModel) << "SearchString: " << _searchString << "resulted in reply: " << reply;

    QVector<ShareePtr> newSharees;

    const QStringList shareeTypes {"users", "groups", "emails", "remotes", "circles", "rooms"};

    const auto appendSharees = [this, &shareeTypes, &newSharees](const QJsonObject &data) {
        for (const auto &shareeType : shareeTypes) {
            const auto category = data.value(shareeType).toArray();

            for (const auto &sharee : category) {
                const auto shareeJsonObject = sharee.toObject();
                const auto parsedSharee = parseSharee(shareeJsonObject);

                // Filter sharees that we have already shared with
                const auto shareeInBlacklistIt = std::find_if(_shareeBlacklist.cbegin(),
                                                              _shareeBlacklist.cend(),
                                                              [&parsedSharee](const ShareePtr &blacklistSharee) {
                    return parsedSharee->type() == blacklistSharee->type() &&
                           parsedSharee->shareWith() == blacklistSharee->shareWith();
                });

                if (shareeInBlacklistIt != _shareeBlacklist.cend()) {
                    continue;
                }

                newSharees.append(parsedSharee);
            }
        }
    };
    const auto replyDataObject = reply.object().value("ocs").toObject().value("data").toObject();
    const auto replyDataExactMatchObject = replyDataObject.value("exact").toObject();

    appendSharees(replyDataObject);
    appendSharees(replyDataExactMatchObject);

    Q_EMIT beginResetModel();
    _sharees = newSharees;
    Q_EMIT endResetModel();

    Q_EMIT shareesReady();
}

ShareePtr ShareeModel::parseSharee(const QJsonObject &data) const
{
    auto displayName = data.value("label").toString();
    const auto shareWith = data.value("value").toObject().value("shareWith").toString();
    const auto type = (Sharee::Type)data.value("value").toObject().value("shareType").toInt();
    const auto additionalInfo = data.value("value").toObject().value("shareWithAdditionalInfo").toString();
    if (!additionalInfo.isEmpty()) {
        displayName = tr("%1 (%2)", "sharee (shareWithAdditionalInfo)").arg(displayName, additionalInfo);
    }

    return ShareePtr(new Sharee(shareWith, displayName, type));
}

}
