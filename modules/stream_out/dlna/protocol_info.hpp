/*****************************************************************************
 * protocol_info.hpp : DLNA renderer protocol matching
 *****************************************************************************
 * Copyright © 2018 VLC authors and VideoLAN
 *
 * Authors: Shaleen Jain <shaleen@jain.sh>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifndef DLNA_PROTOCOL_INFO_H
#define DLNA_PROTOCOL_INFO_H

#include "profile_names.hpp"
#include <vlc_es.h>
#include <vlc_bits.h>
#include <vlc_tick.h>

/* UPnP REL_TIME targets are offsets from the beginning of the resource. */
static inline std::string dlna_seek_time(vlc_tick_t time)
{
    if (time < 0)
        return {};
    const uint64_t seconds = SEC_FROM_VLC_TICK(time);
    std::string hours = std::to_string(seconds / 3600);
    if (hours.size() < 2)
        hours.insert(0, 1, '0');
    const unsigned minutes = seconds / 60 % 60;
    const unsigned remainder = seconds % 60;
    return hours + ":" + (minutes < 10 ? "0" : "") + std::to_string(minutes)
        + ":" + (remainder < 10 ? "0" : "") + std::to_string(remainder);
}

struct protocol_info_t {
    protocol_info_t() = default;
    protocol_info_t(const protocol_info_t&) = default;

    // aggregate initializers do not work in c++11
    // for class types with default member initializers
    protocol_info_t(dlna_transport_protocol_t transport,
            dlna_org_conversion_t ci,
            dlna_profile_t profile)
        : transport(transport)
        , ci(ci)
        , profile(profile)
        {}

    dlna_transport_protocol_t transport = DLNA_TRANSPORT_PROTOCOL_HTTP;
    dlna_org_conversion_t ci = DLNA_ORG_CONVERSION_NONE;
    dlna_profile_t profile;
    dlna_org_operation_t operation = DLNA_ORG_OPERATION_NONE;

    /* Exact DLNA declarations outrank MIME-only wildcard expansions. */
    bool exact_match = false;
    std::string renderer_protocol;
};

/* An absent/invalid profile is not a wildcard. Match whole parameter names,
 * so unrelated vendor parameters cannot masquerade as DLNA.ORG_PN. */
static inline std::string dlna_parse_profile_name(const std::string &info)
{
    if (info == "*")
        return info;

    const std::string tag = "DLNA.ORG_PN=";
    std::string name;
    bool found = false;
    for (std::size_t start = 0; start < info.size(); )
    {
        const std::size_t end = info.find(';', start);
        const std::string field = info.substr(start,
            end == std::string::npos ? end : end - start);
        if (field.compare(0, tag.size(), tag) == 0)
        {
            if (found)
                return {};
            found = true;
            name = field.substr(tag.size());
            if (name.empty() || name.find_first_not_of(
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_")
                    != std::string::npos)
                return {};
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return name;
}

static inline std::vector<protocol_info_t>
dlna_parse_protocols(const std::string &csv)
{
    std::vector<protocol_info_t> protocols;
    for (std::size_t start = 0; start < csv.size(); )
    {
        const std::size_t end = csv.find(',', start);
        const std::string entry = csv.substr(start,
            end == std::string::npos ? end : end - start);
        const std::size_t transport = entry.find(':');
        const std::size_t network = transport == std::string::npos ? transport
            : entry.find(':', transport + 1);
        const std::size_t mime_end = network == std::string::npos ? network
            : entry.find(':', network + 1);
        if (mime_end != std::string::npos
            && entry.find(':', mime_end + 1) == std::string::npos
            && entry.substr(0, transport) == "http-get")
        {
            const std::string mime = entry.substr(network + 1, mime_end - network - 1);
            const std::string name = dlna_parse_profile_name(entry.substr(mime_end + 1));
            if (name == "*")
            {
                protocol_info_t generic;
                generic.profile = dlna_profile_t("*", "", mime,
                    mime.compare(0, 6, "audio/") == 0 ? DLNA_CLASS_AUDIO : DLNA_CLASS_AV,
                    VLC_CODEC_UNKNOWN, VLC_CODEC_UNKNOWN);
                generic.renderer_protocol = entry;
                protocols.push_back(std::move(generic));
            }
            if (!name.empty())
                for (const auto &profile : dlna_profile_list)
                    if (mime == profile.mime && (name == "*" || name == profile.name))
                    {
                        protocol_info_t protocol;
                        protocol.profile = profile;
                        protocol.exact_match = name != "*";
                        /* A MIME wildcard does not assert any named profile. */
                        if (!protocol.exact_match)
                            protocol.profile.name = "*";
                        protocol.renderer_protocol = entry;
                        protocols.push_back(std::move(protocol));
                    }
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return protocols;
}

static inline bool dlna_aac_lc(const es_format_t *audio)
{
    if (audio->i_profile != -1 && audio->i_profile != 1)
        return false;
    if (!audio->p_extra || audio->i_extra < 2)
        return audio->i_profile == 1;
    bs_t bits;
    bs_init(&bits, audio->p_extra, audio->i_extra);
    if (bs_read(&bits, 5) != 2) // AAC-LC audio object type.
        return false;
    unsigned index = bs_read(&bits, 4);
    if (index == 15)
        bs_skip(&bits, 24);
    unsigned channels = bs_read(&bits, 4);
    if (channels == 0 || channels > 2)
        return false; // PCE and multichannel configurations are not modelled.
    if (bs_read1(&bits) || bs_read1(&bits) || bs_read1(&bits))
        return false; // 960-sample frames, core dependency or GA extension.
    if (!bs_error(&bits) && audio->i_extra <= SIZE_MAX / 8
        && audio->i_extra * 8 - bs_pos(&bits) >= 17
        && bs_read(&bits, 11) == 0x2b7
        && bs_read(&bits, 5) == 5 && bs_read1(&bits))
        return false; // Implicit SBR is HE-AAC, despite an AAC-LC core.
    return !bs_error(&bits);
}

/* Main@L4 implies an AVC bitrate ceiling of 20 Mb/s even when the packetizer
 * does not report i_bitrate. AC-3 likewise has a codec maximum of 640 kb/s.
 * Only claim a named AVC profile when its constraints are modelled. Unknown
 * metadata falls back to the renderer's MIME wildcard, without a guessed PN.
 * This intentionally accepts a conservative subset of the HD TS profiles. */
static inline bool dlna_profile_matches(const protocol_info_t &protocol,
                                       const es_format_t *audio,
                                       const es_format_t *video)
{
    if (!protocol.exact_match || !video || video->i_codec != VLC_CODEC_H264)
        return true;
    if (protocol.profile.constraints != DLNA_CONSTRAINTS_AVC_MAIN_HD_TS || !audio)
        return false;

    const auto &v = video->video;
    const unsigned width = v.i_visible_width ? v.i_visible_width : v.i_width;
    const unsigned height = v.i_visible_height ? v.i_visible_height : v.i_height;
    if (video->i_profile != 77 || video->i_level != 40
        || !width || !height || width > 1920 || height > 1080
        || !v.i_frame_rate || !v.i_frame_rate_base
        || video->i_bitrate > 20000000)
        return false;
    const unsigned max_fps = width <= 1280 && height <= 720 ? 60 : 30;
    if (uint64_t(v.i_frame_rate) > uint64_t(max_fps) * v.i_frame_rate_base)
        return false;
    if (!audio->audio.i_channels || audio->audio.i_channels > 6
        || (audio->audio.i_rate != 32000 && audio->audio.i_rate != 44100
            && audio->audio.i_rate != 48000))
        return false;
    if (audio->i_codec == VLC_CODEC_A52)
        return audio->i_bitrate <= 640000;
    /* AAC-LC stereo is a subset of AAC_MULT5. Other AAC configurations need
     * their own validation, including MPEG-4 audio object type and level. */
    /* AAC-LC stereo at <=48 kHz is below AAC_MULT5's 1.44 Mb/s limit
     * even at its maximum 6144 bits per channel per 1024-sample frame. */
    return audio->i_codec == VLC_CODEC_MP4A && dlna_aac_lc(audio)
        && audio->audio.i_channels <= 2 && audio->i_bitrate <= 576000;
}

static inline const protocol_info_t *
dlna_select_protocol(const std::vector<protocol_info_t> &protocols,
                     dlna_media_class_t media, vlc_fourcc_t audio,
                     vlc_fourcc_t video = VLC_CODEC_NONE,
                     const es_format_t *audio_format = nullptr,
                     const es_format_t *video_format = nullptr)
{
    const protocol_info_t *best = nullptr;
    for (const auto &protocol : protocols)
    {
        if (protocol.profile.media != media || (protocol.profile.audio_codec != audio
                && !(media == DLNA_CLASS_AV && audio == VLC_CODEC_NONE
                     && !protocol.exact_match))
            || (media == DLNA_CLASS_AV && protocol.profile.video_codec != video))
            continue;
        if (!dlna_profile_matches(protocol, audio_format, video_format))
            continue;
        /* The TS mux emits ISO packets, not timestamped TTS packets. */
        if (protocol.profile.mux == "ts" && protocol.profile.mime != "video/mpeg")
            continue;
        if (!best || (protocol.exact_match && !best->exact_match)
            || (!protocol.exact_match && !best->exact_match && media == DLNA_CLASS_AV
                && protocol.profile.mux == "ts" && best->profile.mux != "ts"))
            best = &protocol;
    }
    return best;
}

#endif /* DLNA_PROTOCOL_INFO_H */
