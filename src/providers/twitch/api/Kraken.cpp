#include "providers/twitch/api/Kraken.hpp"

#include "common/NetworkResult.hpp"
#include "common/Outcome.hpp"
#include "common/QLogging.hpp"
#include "providers/twitch/TwitchCommon.hpp"

namespace chatterino {

static Kraken *instance = nullptr;

void Kraken::followUser(QString userId, QString targetId,
                        std::function<void()> successCallback,
                        KrakenFailureCallback failureCallback)
{
    this->makeRequest(
            QString("users/%1/follows/channels/%2").arg(userId, targetId), {},
            NetworkRequestType::Put)
        .onSuccess([successCallback](auto result) -> Outcome {
            successCallback();

            return Success;
        })
        .onError([failureCallback](auto result) {
            failureCallback();
        })
        .execute();
}

void Kraken::unfollowUser(QString userName, QString targetName,
                          std::function<void()> successCallback,
                          KrakenFailureCallback failureCallback)
{
    this->makeRequest(
            QString("users/%1/follows/channels/%2").arg(userName, targetName),
            {}, NetworkRequestType::Delete)
        .onSuccess([successCallback](auto result) -> Outcome {
            successCallback();

            return Success;
        })
        .onError([failureCallback](auto result) {
            failureCallback();
        })
        .execute();
}

NetworkRequest Kraken::makeRequest(QString url, QUrlQuery urlQuery,
                                   NetworkRequestType type)
{
    assert(!url.startsWith("/"));

    if (this->clientId.isEmpty())
    {
        qCDebug(chatterinoTwitch)
            << "Kraken::makeRequest called without a client ID set BabyRage";
    }

    const QString baseUrl("https://api.twitch.tv/kraken/");

    QUrl fullUrl(baseUrl + url);

    fullUrl.setQuery(urlQuery);

    if (!this->oauthToken.isEmpty())
    {
        return NetworkRequest(fullUrl, type)
            .timeout(5 * 1000)
            .header("Accept", "application/vnd.twitchtv.v3+json")
            .header("Client-ID", this->clientId)
            .header("Authorization", "OAuth " + this->oauthToken);
    }

    return NetworkRequest(fullUrl, type)
        .timeout(5 * 1000)
        .header("Accept", "application/vnd.twitchtv.v3+json")
        .header("Client-ID", this->clientId);
}

void Kraken::update(QString clientId, QString oauthToken)
{
    this->clientId = std::move(clientId);
    this->oauthToken = std::move(oauthToken);
}

void Kraken::initialize()
{
    assert(instance == nullptr);

    instance = new Kraken();

    getKraken()->update(getDefaultClientID(), "");
}

Kraken *getKraken()
{
    assert(instance != nullptr);

    return instance;
}

}  // namespace chatterino
