/*****************************************************************************
 * dlna.cpp : DLNA protocol parsing and selection tests
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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#undef NDEBUG
#include <cassert>
#include "../../../modules/stream_out/dlna/protocol_info.hpp"
#include "../../../src/network/httpd_file.h"

static void test_profile_name()
{
    assert(dlna_parse_profile_name("DLNA.ORG_PN=FOO;DLNA.ORG_FLAGS=00") == "FOO");
    assert(dlna_parse_profile_name("DLNA.ORG_PN=FOO") == "FOO");
    assert(dlna_parse_profile_name("*") == "*");
    assert(dlna_parse_profile_name("DLNA.ORG_OP=01;DLNA.ORG_PN=FOO;X=Y") == "FOO");
    assert(dlna_parse_profile_name("DLNA.ORG_OP=01;DLNA.ORG_FLAGS=00").empty());
    assert(dlna_parse_profile_name("X_DLNA.ORG_PN=FOO").empty());
    assert(dlna_parse_profile_name("X=DLNA.ORG_PN=FOO").empty());
    assert(dlna_parse_profile_name("DLNA.ORG_PN=").empty());
    assert(dlna_parse_profile_name("DLNA.ORG_PN=;DLNA.ORG_OP=01").empty());
    assert(dlna_parse_profile_name("DLNA.ORG_PN==FOO").empty());
    assert(dlna_parse_profile_name("DLNA.ORG_PN=*").empty());
    assert(dlna_parse_profile_name("DLNA.ORG_PN=FOO;DLNA.ORG_PN=BAR").empty());
    assert(dlna_parse_profile_name("").empty());
}

static void test_protocols()
{
    const auto exact = dlna_parse_protocols(
        "http-get:*:video/mpeg:DLNA.ORG_PN=AVC_TS_NA_ISO;DLNA.ORG_OP=01");
    assert(!exact.empty());
    for (const auto &protocol : exact)
    {
        assert(protocol.exact_match);
        assert(protocol.profile.name == "AVC_TS_NA_ISO");
        assert(protocol.profile.mime == "video/mpeg");
        assert(!protocol.renderer_protocol.empty());
    }
    assert(dlna_parse_protocols(
        "http-get:*:video/mpeg:DLNA.ORG_PN=AVC_TS_NA_ISO").size() == exact.size());
    assert(dlna_parse_protocols("http-get:*:audio/mp4:DLNA.ORG_PN=AVC_TS_NA_ISO").empty());
    assert(dlna_parse_protocols("rtsp-rtp-udp:*:video/mpeg:*").empty());
    assert(dlna_parse_protocols("http-get:*:video/mpeg").empty());
    assert(dlna_parse_protocols("http-get:*:video/mpeg:*:extra").empty());
    assert(dlna_parse_protocols("http-get:*:video/mpeg:").empty());
    assert(dlna_parse_protocols("http-get:*:video/mpeg:DLNA.ORG_PN=").empty());
    assert(dlna_parse_protocols("http-get:*:video/mpeg:DLNA.ORG_OP=01").empty());
    assert(dlna_parse_protocols("http-get:*:video/mpeg:DLNA.ORG_PN=UNKNOWN").empty());
    assert(dlna_parse_protocols("").empty());

    const auto wildcard = dlna_parse_protocols("http-get:*:video/mp4:*");
    assert(!wildcard.empty());
    for (const auto &protocol : wildcard)
        assert(!protocol.exact_match);
    const auto *fallback = dlna_select_protocol(wildcard, DLNA_CLASS_AV,
                                              VLC_CODEC_A52, VLC_CODEC_H264);
    assert(fallback && !fallback->exact_match);
    assert(fallback->profile.mime == "video/mp4");
    assert(!dlna_select_protocol(wildcard, DLNA_CLASS_AV,
                                VLC_CODEC_FLAC, VLC_CODEC_H264));
}

static void test_ranking()
{
    const std::string wildcard = "http-get:*:video/mp4:*,http-get:*:video/mpeg:*";
    const std::string exact = "http-get:*:video/mpeg:DLNA.ORG_PN=AVC_TS_NA_ISO";
    for (const auto &csv : {wildcard + "," + exact, exact + "," + wildcard})
    {
        const auto protocols = dlna_parse_protocols(csv);
        const auto *selected = dlna_select_protocol(protocols, DLNA_CLASS_AV,
                                                   VLC_CODEC_A52, VLC_CODEC_H264);
        assert(selected && selected->exact_match);
        assert(selected->profile.name == "AVC_TS_NA_ISO");
        assert(selected->profile.mux == "ts");
        assert(selected->profile.mime == "video/mpeg");
    }

    /* An incompatible exact declaration must not suppress a usable fallback. */
    const auto incompatible = dlna_parse_protocols(wildcard +
        ",http-get:*:video/mp4:DLNA.ORG_PN=AVC_MP4_MP_SD");
    const auto *selected = dlna_select_protocol(incompatible, DLNA_CLASS_AV,
                                               VLC_CODEC_A52, VLC_CODEC_H264);
    assert(selected && !selected->exact_match);

    const auto audio = dlna_parse_protocols(
        "http-get:*:audio/mp4:*,http-get:*:audio/mp4:DLNA.ORG_PN=AAC_ISO_320");
    selected = dlna_select_protocol(audio, DLNA_CLASS_AUDIO, VLC_CODEC_MP4A);
    assert(selected && selected->exact_match);
    assert(selected->profile.name == "AAC_ISO_320");
}

static void test_formats()
{
    es_format_t video = {};
    video.i_codec = VLC_CODEC_H264;
    video.i_profile = 77;
    video.i_level = 40;
    video.i_bitrate = 10000000;
    video.video.i_width = 1920;
    video.video.i_height = 1080;
    video.video.i_frame_rate = 30;
    video.video.i_frame_rate_base = 1;
    es_format_t audio = {};
    audio.i_codec = VLC_CODEC_A52;
    audio.i_bitrate = 384000;
    audio.audio.i_channels = 6;
    audio.audio.i_rate = 48000;
    const auto protocols = dlna_parse_protocols(
        "http-get:*:video/mp4:*,http-get:*:video/mpeg:*,"
        "http-get:*:video/mpeg:DLNA.ORG_PN=AVC_TS_MP_HD_AC3_ISO");
    auto select = [&]() {
        return dlna_select_protocol(protocols, DLNA_CLASS_AV,
                                    audio.i_codec, video.i_codec, &audio, &video);
    };
    auto *selected = select();
    assert(selected && selected->exact_match);
    assert(selected->profile.name == "AVC_TS_MP_HD_AC3_ISO");
    assert(selected->profile.mux == "ts");
    video.video.i_height = 1088;
    video.video.i_visible_height = 1080;
    assert(select()->exact_match); // H.264 macroblock padding is not visible HD height.
    video.video.i_height = 1080;
    video.video.i_visible_height = 0;

    video.i_profile = 100; // High profile is not Main profile.
    selected = select();
    assert(selected && !selected->exact_match && selected->profile.name == "*");
    assert(selected->profile.mux == "ts");
    video.i_profile = 77;
    video.i_level = 41;
    assert(!select()->exact_match);
    video.i_level = 40;
    video.video.i_width = 3840;
    assert(!select()->exact_match);
    video.video.i_width = 1920;
    video.video.i_frame_rate = 60;
    assert(!select()->exact_match);
    video.video.i_width = 1280;
    video.video.i_height = 720;
    assert(select()->exact_match);
    video.i_bitrate = 21000000;
    assert(!select()->exact_match);
    video.i_bitrate = 10000000;
    audio.audio.i_channels = 8;
    assert(!select()->exact_match);
    audio.audio.i_channels = 6;
    audio.audio.i_rate = 96000;
    assert(!select()->exact_match);
    audio.audio.i_rate = 48000;
    audio.i_bitrate = 0;
    assert(select()->exact_match); // AC-3 codec maximum is within the profile.

    /* Do not label HD video with any unvalidated SD or CIF profile. */
    const auto sd = dlna_parse_protocols(
        "http-get:*:video/mp4:DLNA.ORG_PN=AVC_MP4_MP_SD,"
        "http-get:*:video/mp4:*");
    audio.i_codec = VLC_CODEC_MP4A;
    selected = dlna_select_protocol(sd, DLNA_CLASS_AV,
        audio.i_codec, video.i_codec, &audio, &video);
    assert(selected && !selected->exact_match && selected->profile.name == "*");

    const auto aac = dlna_parse_protocols(
        "http-get:*:video/mpeg:DLNA.ORG_PN=AVC_TS_MP_HD_AAC_MULT5_ISO");
    audio.i_profile = 1;
    audio.i_bitrate = 128000;
    audio.audio.i_channels = 2;
    selected = dlna_select_protocol(aac, DLNA_CLASS_AV,
        audio.i_codec, video.i_codec, &audio, &video);
    assert(selected && selected->exact_match);
    uint8_t asc[] = {0x11, 0x90}; // AAC-LC, 48 kHz, stereo.
    audio.i_profile = -1;
    audio.i_extra = sizeof(asc);
    audio.p_extra = asc;
    video.i_bitrate = 0;
    audio.i_bitrate = 0;
    assert(dlna_select_protocol(aac, DLNA_CLASS_AV,
        audio.i_codec, video.i_codec, &audio, &video));
    uint8_t sbr_asc[] = {0x11, 0x90, 0x56, 0xe5, 0x80};
    audio.p_extra = sbr_asc;
    audio.i_extra = sizeof(sbr_asc);
    assert(!dlna_aac_lc(&audio));
    audio.p_extra = asc;
    audio.i_extra = sizeof(asc);
    audio.i_profile = 4; // HE-AAC must not be treated as AAC-LC.
    assert(!dlna_select_protocol(aac, DLNA_CLASS_AV,
        audio.i_codec, video.i_codec, &audio, &video));

    /* MIME wildcard permits video-only remux without inventing an audio ES. */
    selected = dlna_select_protocol(protocols, DLNA_CLASS_AV,
        VLC_CODEC_NONE, video.i_codec, nullptr, &video);
    assert(selected && !selected->exact_match && selected->profile.mux == "ts");
}

static void test_ranges()
{
    uint64_t start, length;
    assert(httpd_file_range(nullptr, 1000, &start, &length));
    assert(start == 0 && length == 1000);
    assert(httpd_file_range("bytes=0-", 1000, &start, &length));
    assert(start == 0 && length == 1000);
    assert(httpd_file_range("bytes=100-199", 1000, &start, &length));
    assert(start == 100 && length == 100);
    assert(httpd_file_range("bytes=999-9999", 1000, &start, &length));
    assert(start == 999 && length == 1);
    assert(httpd_file_range("bytes=-100", 1000, &start, &length));
    assert(start == 900 && length == 100);
    assert(httpd_file_range("bytes=-9999", 1000, &start, &length));
    assert(start == 0 && length == 1000);
    assert(httpd_file_range("", 0, &start, &length));
    assert(length == 0);
    for (const char *range : {"bytes=1000-", "bytes=10-1", "bytes=-0",
            "bytes=1-2,4-5", "bytes=+1-", "bytes=", "bytes=a-",
            "bytes=0-1junk", "bytes=999999999999999999999999-", "items=0-1"})
        assert(!httpd_file_range(range, 1000, &start, &length));
    assert(!httpd_file_range("bytes=0-", 0, &start, &length));
}

int main()
{
    assert(dlna_seek_time(-1).empty());
    assert(dlna_seek_time(0) == "00:00:00");
    assert(dlna_seek_time(VLC_TICK_FROM_MS(59999)) == "00:00:59");
    assert(dlna_seek_time(VLC_TICK_FROM_SEC(3601)) == "01:00:01");
    assert(dlna_seek_time(VLC_TICK_FROM_SEC(10596)) == "02:56:36");
    test_profile_name();
    test_protocols();
    test_ranking();
    test_formats();
    test_ranges();
}
