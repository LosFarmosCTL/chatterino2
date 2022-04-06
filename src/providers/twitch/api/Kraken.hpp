#pragma once

#include "common/NetworkRequest.hpp"
#include "providers/twitch/TwitchAccount.hpp"

#include <QString>
#include <QStringList>
#include <QUrlQuery>

#include <functional>

namespace chatterino {

using KrakenFailureCallback = std::function<void()>;
template <typename... T>
using ResultCallback = std::function<void(T...)>;

class Kraken final : boost::noncopyable
{
public:
    // https://dev.twitch.tv/docs/api/reference#create-user-follows
    void followUser(QString userId, QString targetId,
                    std::function<void()> successCallback,
                    KrakenFailureCallback);

    // https://dev.twitch.tv/docs/api/reference#delete-user-follows
    void unfollowUser(QString userId, QString targetlId,
                      std::function<void()> successCallback,
                      KrakenFailureCallback);

    void update(QString clientId, QString oauthToken);

    static void initialize();

private:
    NetworkRequest makeRequest(QString url, QUrlQuery urlQuery, NetworkRequestType type);

    QString clientId;
    QString oauthToken;
};

Kraken *getKraken();

}  // namespace chatterino
