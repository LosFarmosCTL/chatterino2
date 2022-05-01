#include "providers/twitch/TwitchAccount.hpp"

#include <QThread>

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/Env.hpp"
#include "common/NetworkRequest.hpp"
#include "common/Outcome.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/IvrApi.hpp"
#include "providers/irc/IrcMessageBuilder.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/twitch/TwitchUser.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "singletons/Emotes.hpp"
#include "util/QStringHash.hpp"
#include "util/RapidjsonHelpers.hpp"

namespace chatterino {

std::vector<QStringList> getEmoteSetBatches(QStringList emoteSetKeys)
{
    // splitting emoteSetKeys to batches of 25, because the Helix endpoint accepts a maximum of 25 emotesets at once
    constexpr int batchSize = 25;

    int batchCount = (emoteSetKeys.size() / batchSize) + 1;

    std::vector<QStringList> batches;
    batches.reserve(batchCount);

    for (int i = 0; i < batchCount; i++)
    {
        QStringList batch;

        int last = std::min(batchSize, emoteSetKeys.size() - batchSize * i);
        for (int j = 0; j < last; j++)
        {
            batch.push_back(emoteSetKeys.at(j + (batchSize * i)));
        }
        batches.emplace_back(batch);
    }

    return batches;
}

TwitchAccount::TwitchAccount(const QString &username, const QString &oauthToken,
                             const QString &oauthClient, const QString &userID)
    : Account(ProviderId::Twitch)
    , oauthClient_(oauthClient)
    , oauthToken_(oauthToken)
    , userName_(username)
    , userId_(userID)
    , isAnon_(username == ANONYMOUS_USERNAME)
{
}

QString TwitchAccount::toString() const
{
    return this->getUserName();
}

const QString &TwitchAccount::getUserName() const
{
    return this->userName_;
}

const QString &TwitchAccount::getOAuthClient() const
{
    return this->oauthClient_;
}

const QString &TwitchAccount::getOAuthToken() const
{
    return this->oauthToken_;
}

const QString &TwitchAccount::getUserId() const
{
    return this->userId_;
}

QColor TwitchAccount::color()
{
    return this->color_.get();
}

void TwitchAccount::setColor(QColor color)
{
    this->color_.set(std::move(color));
}

bool TwitchAccount::setOAuthClient(const QString &newClientID)
{
    if (this->oauthClient_.compare(newClientID) == 0)
    {
        return false;
    }

    this->oauthClient_ = newClientID;

    return true;
}

bool TwitchAccount::setOAuthToken(const QString &newOAuthToken)
{
    if (this->oauthToken_.compare(newOAuthToken) == 0)
    {
        return false;
    }

    this->oauthToken_ = newOAuthToken;

    return true;
}

bool TwitchAccount::isAnon() const
{
    return this->isAnon_;
}

void TwitchAccount::loadBlocks()
{
    getHelix()->loadBlocks(
        getApp()->accounts->twitch.getCurrent()->userId_,
        [this](std::vector<HelixBlock> blocks) {
            auto ignores = this->ignores_.access();
            auto userIds = this->ignoresUserIds_.access();
            ignores->clear();
            userIds->clear();

            for (const HelixBlock &block : blocks)
            {
                TwitchUser blockedUser;
                blockedUser.fromHelixBlock(block);
                ignores->insert(blockedUser);
                userIds->insert(blockedUser.id);
            }
        },
        [] {
            qCWarning(chatterinoTwitch) << "Fetching blocks failed!";
        });
}

void TwitchAccount::blockUser(QString userId, std::function<void()> onSuccess,
                              std::function<void()> onFailure)
{
    getHelix()->blockUser(
        userId,
        [this, userId, onSuccess] {
            TwitchUser blockedUser;
            blockedUser.id = userId;
            {
                auto ignores = this->ignores_.access();
                auto userIds = this->ignoresUserIds_.access();

                ignores->insert(blockedUser);
                userIds->insert(blockedUser.id);
            }
            onSuccess();
        },
        std::move(onFailure));
}

void TwitchAccount::unblockUser(QString userId, std::function<void()> onSuccess,
                                std::function<void()> onFailure)
{
    getHelix()->unblockUser(
        userId,
        [this, userId, onSuccess] {
            TwitchUser ignoredUser;
            ignoredUser.id = userId;
            {
                auto ignores = this->ignores_.access();
                auto userIds = this->ignoresUserIds_.access();

                ignores->erase(ignoredUser);
                userIds->erase(ignoredUser.id);
            }
            onSuccess();
        },
        std::move(onFailure));
}

SharedAccessGuard<const std::set<TwitchUser>> TwitchAccount::accessBlocks()
    const
{
    return this->ignores_.accessConst();
}

SharedAccessGuard<const std::set<QString>> TwitchAccount::accessBlockedUserIds()
    const
{
    return this->ignoresUserIds_.accessConst();
}

void TwitchAccount::loadEmotes(std::weak_ptr<Channel> weakChannel)
{
    qCDebug(chatterinoTwitch)
        << "Loading Twitch emotes for user" << this->getUserName();

    if (this->getOAuthClient().isEmpty() || this->getOAuthToken().isEmpty())
    {
        qCDebug(chatterinoTwitch)
            << "Aborted loadEmotes due to missing Client ID and/or OAuth token";
        return;
    }

    {
        auto emoteData = this->emotes_.access();
        emoteData->emoteSets.clear();
        emoteData->emotes.clear();
        qCDebug(chatterinoTwitch) << "Cleared emotes!";
    }

    this->loadUserstateEmotes(weakChannel);
}

bool TwitchAccount::setUserstateEmoteSets(QStringList newEmoteSets)
{
    newEmoteSets.sort();

    if (this->userstateEmoteSets_ == newEmoteSets)
    {
        // Nothing has changed
        return false;
    }

    this->userstateEmoteSets_ = newEmoteSets;

    return true;
}

TwitchAccount::EmoteType TwitchAccount::getEmoteTypeFromHelixResponse(
    const QString emoteSetType)
{
    if (emoteSetType == "globals")
    {
        return TwitchAccount::EmoteType::Global;
    }
    else if (emoteSetType == "smilies")
    {
        return TwitchAccount::EmoteType::Smilies;
    }
    else if (emoteSetType == "subscriptions")
    {
        return TwitchAccount::EmoteType::Subscription;
    }
    else if (emoteSetType == "bitstier")
    {
        return TwitchAccount::EmoteType::Bits;
    }
    else if (emoteSetType == "follower")
    {
        return TwitchAccount::EmoteType::Follower;
    }

    return TwitchAccount::EmoteType::Special;
}

void TwitchAccount::loadUserstateEmotes(std::weak_ptr<Channel> weakChannel)
{
    if (this->userstateEmoteSets_.isEmpty())
    {
        return;
    }

    QStringList newEmoteSetKeys, existingEmoteSetKeys;

    auto emoteData = this->emotes_.access();
    auto userEmoteSets = emoteData->emoteSets;

    // get list of already fetched emote sets
    for (const auto &userEmoteSet : userEmoteSets)
    {
        for (const auto &setId : userEmoteSet->setIds)
        {
            existingEmoteSetKeys.push_back(setId);
        }
    }

    // filter out emote sets from userstate message, which are not in fetched emote set list
    for (const auto &emoteSetKey : qAsConst(this->userstateEmoteSets_))
    {
        if (!existingEmoteSetKeys.contains(emoteSetKey))
        {
            newEmoteSetKeys.push_back(emoteSetKey);
        }
    }

    // return if there are no new emote sets
    if (newEmoteSetKeys.isEmpty())
    {
        return;
    }

    // requesting emotes
    auto batches = getEmoteSetBatches(newEmoteSetKeys);
    for (int i = 0; i < batches.size(); i++)
    {
        qCDebug(chatterinoTwitch)
            << QString(
                   "Loading %1 emotesets from Helix; batch %2/%3 (%4 sets): %5")
                   .arg(newEmoteSetKeys.size())
                   .arg(i + 1)
                   .arg(batches.size())
                   .arg(batches.at(i).size())
                   .arg(batches.at(i).join(","));

        getHelix()->fetchEmoteSets(
            batches.at(i),
            [this, weakChannel](const std::vector<HelixEmote> &helixEmotes) {
                QStringList emoteOwnerIds;
                for (const auto &helixEmote : helixEmotes)
                {
                    auto alreadyAdded =
                        std::find_if(emoteOwnerIds.begin(), emoteOwnerIds.end(),
                                     [&helixEmote](const auto &ownerId) {
                                         return helixEmote.ownerId == ownerId;
                                     });

                    if (alreadyAdded != emoteOwnerIds.end())
                    {
                        continue;
                    }

                    // twitch is used as owner id for smilies and would result in the user lookup to fail
                    if (helixEmote.ownerId == "twitch")
                    {
                        continue;
                    }

                    emoteOwnerIds << helixEmote.ownerId;
                }

                // Helix does not provide display/login name for emote set owners, so we have to look them up with another API call
                getHelix()->fetchUsersById(
                    emoteOwnerIds,
                    [this, weakChannel,
                     helixEmotes](const std::vector<HelixUser> &users) {
                        auto emoteData = this->emotes_.access();
                        auto localEmoteData = this->localEmotes_.access();

                        std::vector<std::shared_ptr<EmoteSet>> emoteSets;
                        for (const auto &helixEmote : helixEmotes)
                        {
                            auto emoteType =
                                getEmoteTypeFromHelixResponse(helixEmote.type);

                            // TODO: investigate smilies
                            auto existingEmoteSet = std::find_if(
                                emoteSets.begin(), emoteSets.end(),
                                [&helixEmote, emoteType](const auto &emoteSet) {
                                    // dont group together all subscription tiers, since the get-emote-sets endpoint does not provide tier information, use the setID
                                    if (emoteType == EmoteType::Subscription)
                                    {
                                        return emoteSet->setIds.contains(
                                            helixEmote.setId);
                                    }

                                    //try to group by owner and type instead of set id, to avoid having unlockable (i.e. Haha, 2020, ...) emotes all in a seperate group
                                    return emoteSet->ownerId ==
                                               helixEmote.ownerId &&
                                           emoteSet->type == emoteType;
                                });

                            std::shared_ptr<EmoteSet> emoteSet;
                            if (existingEmoteSet != emoteSets.end())
                            {
                                emoteSet = *existingEmoteSet;

                                if (!emoteSet->setIds.contains(
                                        helixEmote.setId))
                                {
                                    emoteSet->setIds << helixEmote.setId;
                                }
                            }
                            else
                            {
                                emoteSet = std::make_shared<EmoteSet>();

                                emoteSet->ownerId = helixEmote.ownerId;
                                emoteSet->setIds << helixEmote.setId;
                                emoteSet->type = emoteType;

                                switch (emoteType)
                                {
                                    case EmoteType::Global:
                                    case EmoteType::Smilies: {
                                        emoteSet->ownerName = "twitch";
                                        emoteSet->setName = "Twitch";
                                        break;
                                    }
                                    case EmoteType::Subscription:
                                    case EmoteType::Bits:
                                    case EmoteType::Follower: {
                                        auto emoteOwner = std::find_if(
                                            users.begin(), users.end(),
                                            [&helixEmote](const auto &user) {
                                                return user.id ==
                                                       helixEmote.ownerId;
                                            });

                                        if (emoteOwner == users.end())
                                        {
                                            continue;
                                        }

                                        emoteSet->ownerName = emoteOwner->login;
                                        emoteSet->setName =
                                            emoteOwner->displayName;
                                        break;
                                    }
                                    // These are all the emote types that twitch is currently putting under the qa_TW_Partner acc
                                    case EmoteType::Special: {
                                        emoteSet->ownerName = "twitch";

                                        if (helixEmote.type == "prime")
                                        {
                                            emoteSet->setName = "Prime";
                                        }
                                        else if (helixEmote.type == "twofactor")
                                        {
                                            emoteSet->setName = "2FA";
                                        }
                                        else if (helixEmote.type ==
                                                 "limitedtime")
                                        {
                                            emoteSet->setName = "Limited Time";
                                        }
                                        else if (helixEmote.type == "hypetrain")
                                        {
                                            emoteSet->setName = "Hype Train";
                                        }
                                        else if (helixEmote.type == "rewards")
                                        {
                                            emoteSet->setName = "Rewards";
                                        }
                                        else if (helixEmote.type == "owl2019")
                                        {
                                            emoteSet->setName = "OWL 2019";
                                        }
                                        else
                                        {
                                            emoteSet->setName = "Other";
                                        }
                                    }
                                }

                                emoteSets.emplace_back(emoteSet);
                            }

                            auto emoteId = EmoteId{helixEmote.id};
                            auto emoteCode =
                                EmoteName{TwitchEmotes::cleanUpEmoteCode(
                                    helixEmote.name)};

                            emoteSet->emotes.push_back(
                                TwitchEmote{emoteId, emoteCode});

                            auto emote =
                                getApp()->emotes->twitch.getOrCreateEmote(
                                    emoteId, emoteCode);
                            if (emoteSet->type == EmoteType::Follower)
                            {
                                // EmoteMap for target channel wasn't initialized yet, doing it now
                                if (localEmoteData->find(emoteSet->ownerName) ==
                                    localEmoteData->end())
                                {
                                    localEmoteData->emplace(emoteSet->ownerName,
                                                            EmoteMap());
                                }

                                localEmoteData->at(emoteSet->ownerName)
                                    .emplace(emoteCode, emote);
                            }
                            else
                            {
                                emoteData->emotes.emplace(emoteCode, emote);
                            }
                        }

                        for (const auto &emoteSet : emoteSets)
                        {
                            std::sort(
                                emoteSet->emotes.begin(),
                                emoteSet->emotes.end(),
                                [](const TwitchEmote &l, const TwitchEmote &r) {
                                    return l.name.string < r.name.string;
                                });

                            emoteData->emoteSets.emplace_back(emoteSet);
                        }

                        if (auto channel = weakChannel.lock();
                            channel != nullptr)
                        {
                            channel->addMessage(makeSystemMessage(
                                "Twitch subscriber emotes reloaded."));
                        }
                    },
                    [weakChannel] {
                        // TODO: improve error handling?
                        if (auto channel = weakChannel.lock();
                            channel != nullptr)
                        {
                            channel->addMessage(
                                makeSystemMessage("Failed to fetch Twitch "
                                                  "emotes (unknown error)"));
                        }
                    });
            },
            [weakChannel] {
                if (auto channel = weakChannel.lock(); channel != nullptr)
                {
                    channel->addMessage(makeSystemMessage(
                        "Failed to fetch Twitch emotes (unknown error)"));
                }
            });
    };
}

SharedAccessGuard<const TwitchAccount::TwitchAccountEmoteData>
    TwitchAccount::accessEmotes() const
{
    return this->emotes_.accessConst();
}

SharedAccessGuard<const std::unordered_map<QString, EmoteMap>>
    TwitchAccount::accessLocalEmotes() const
{
    return this->localEmotes_.accessConst();
}

// AutoModActions
void TwitchAccount::autoModAllow(const QString msgID, ChannelPtr channel)
{
    getHelix()->manageAutoModMessages(
        this->getUserId(), msgID, "ALLOW",
        [] {
            // success
        },
        [channel](auto error) {
            // failure
            QString errorMessage("Failed to allow AutoMod message - ");

            switch (error)
            {
                case HelixAutoModMessageError::MessageAlreadyProcessed: {
                    errorMessage += "message has already been processed.";
                }
                break;

                case HelixAutoModMessageError::UserNotAuthenticated: {
                    errorMessage += "you need to re-authenticate.";
                }
                break;

                case HelixAutoModMessageError::UserNotAuthorized: {
                    errorMessage +=
                        "you don't have permission to perform that action";
                }
                break;

                case HelixAutoModMessageError::MessageNotFound: {
                    errorMessage += "target message not found.";
                }
                break;

                // This would most likely happen if the service is down, or if the JSON payload returned has changed format
                case HelixAutoModMessageError::Unknown:
                default: {
                    errorMessage += "an unknown error occured.";
                }
                break;
            }

            channel->addMessage(makeSystemMessage(errorMessage));
        });
}

void TwitchAccount::autoModDeny(const QString msgID, ChannelPtr channel)
{
    getHelix()->manageAutoModMessages(
        this->getUserId(), msgID, "DENY",
        [] {
            // success
        },
        [channel](auto error) {
            // failure
            QString errorMessage("Failed to deny AutoMod message - ");

            switch (error)
            {
                case HelixAutoModMessageError::MessageAlreadyProcessed: {
                    errorMessage += "message has already been processed.";
                }
                break;

                case HelixAutoModMessageError::UserNotAuthenticated: {
                    errorMessage += "you need to re-authenticate.";
                }
                break;

                case HelixAutoModMessageError::UserNotAuthorized: {
                    errorMessage +=
                        "you don't have permission to perform that action";
                }
                break;

                case HelixAutoModMessageError::MessageNotFound: {
                    errorMessage += "target message not found.";
                }
                break;

                // This would most likely happen if the service is down, or if the JSON payload returned has changed format
                case HelixAutoModMessageError::Unknown:
                default: {
                    errorMessage += "an unknown error occured.";
                }
                break;
            }

            channel->addMessage(makeSystemMessage(errorMessage));
        });
}

}  // namespace chatterino
