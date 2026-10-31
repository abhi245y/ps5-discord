// ps5-discord - What screens ask the client to do. The app passes these on to
// the core client (console) or applies them to the sample account (host).
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>

namespace discord
{

struct Request
{
    enum class Kind
    {
        browser_login, // open Discord and import its session on-console
        browser_cancel,
        open_channel, // load history and subscribe to the guild
        load_older,   // the next page of history above what is loaded
        send,         // post content to channel_id, or a sticker (sticker_id) on its own
        react,        // add (or remove) emoji to message_id in channel_id: Unicode, or a server's name:id
        open_dm,      // find or create the DM with user_id (Snapshot::dm_by_user)
        prefetch,     // load history ahead of an open, without marking it read
        edit,         // replace message_id's content (the account's own message)
        remove,       // delete message_id (the account's own message)
        load_sticker_packs, // Discord's own sticker packs, for the picker (Snapshot::sticker_packs)
        remoteplay_pair, // pair Go Live's Remote Play for console user user_id ("" on screen): content = the PIN or ""
        remoteplay_unpair, // forget user_id's pairing
        remoteplay_use, // Go Live runs as console user user_id
        voice_join,   // join a server voice channel: guild_id, channel_id
        voice_call,   // join a DM's call, ringing the other side: channel_id
        voice_answer, // join a DM's call that rings us: channel_id
        voice_decline,// stop ringing us in channel_id
        voice_leave,  // leave whatever we are in
        voice_set,    // self mute and deafen: mute, deaf
        voice_output, // the call's audio while the app is on screen: tv, or the USB controller
        voice_broadcast, // Go Live on or off: on
        set_pref,     // a console-side setting (Prefs): content = its key, value
        sign_out,     // end the session with Discord and forget it on the console
        quit,         // stop the core (no alerts or calls) and close the app
        reconnect,    // Retry now: the core ends its wait before the next attempt
        update_check, // ask the core to look for a new version: on = from Settings (force)
        update_start, // download and unpack the offered version
        update_apply, // swap the files in; the core answers 200 and the app closes itself
        update_restore, // the previous version back: as update_apply
    };
    Kind kind = Kind::open_channel;
    std::string guild_id; // empty for a DM
    std::string channel_id;
    std::string content;
    std::string reply_to; // message id this answers, or empty
    std::string message_id;
    std::string emoji;    // a Unicode emoji, for react
    std::string user_id;  // for open_dm
    std::string nonce;    // set by the client for send: matches the gateway's echo
    bool mute = false;    // voice_set
    bool deaf = false;    // voice_set
    bool tv = false;      // voice_output
    bool on = false;      // voice_broadcast
    bool remove = false;  // react: take this account's reaction off
    std::string sticker_id;   // send: a sticker instead of text
    std::string sticker_name;
    int sticker_format = 1;
    int value = 0;        // set_pref
};

} // namespace discord
