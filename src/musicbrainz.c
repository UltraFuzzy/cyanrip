/*
 * This file is part of cyanrip.
 *
 * cyanrip is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * cyanrip is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with cyanrip; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "musicbrainz.h"
#include "cyanrip_log.h"

#include <musicbrainz5/mb5_c.h>
#include <libavutil/crc.h>

#define READ_MB(FUNC, MBCTX, DICT, KEY, APPEND)                                     \
    do {                                                                            \
        int flags = AV_DICT_DONT_STRDUP_VAL | ((APPEND) ? AV_DICT_APPEND : 0x0);    \
        if (!MBCTX)                                                                 \
            break;                                                                  \
        int len = FUNC(MBCTX, NULL, 0) + 1;                                         \
        char *str = av_mallocz(4*len);                                              \
        FUNC(MBCTX, str, len);                                                      \
        if (str[0] == '\0')                                                         \
            av_free(str);                                                           \
        else                                                                        \
            av_dict_set(&DICT, KEY, str, flags);                                    \
    } while (0)

static void mb_credit(Mb5ArtistCredit credit, AVDictionary *dict, const char *key)
{
    int append = 0;
    Mb5NameCreditList namecredit_list = mb5_artistcredit_get_namecreditlist(credit);

    for (int i = 0; i < mb5_namecredit_list_size(namecredit_list); i++) {
        Mb5NameCredit namecredit = mb5_namecredit_list_item(namecredit_list, i);

        if (mb5_namecredit_get_name(namecredit, NULL, 0)) {
            READ_MB(mb5_namecredit_get_name, namecredit, dict, key, append++);
        } else {
            Mb5Artist artist = mb5_namecredit_get_artist(namecredit);
            if (artist)
                READ_MB(mb5_artist_get_name, artist, dict, key, append++);
        }

        READ_MB(mb5_namecredit_get_joinphrase, namecredit, dict, key, append++);
    }
}

/* This is pretty awful but I blame musicbrainz entirely */
static uint32_t crc_medium(Mb5Medium medium)
{
    uint32_t crc = UINT32_MAX;
    const AVCRC *crc_tab = av_crc_get_table(AV_CRC_32_IEEE_LE);

    Mb5TrackList track_list = mb5_medium_get_tracklist(medium);
    if (!track_list)
        return 0;

    for (int i = 0; i < mb5_track_list_size(track_list); i++) {
        AVDictionary *tmp_dict = NULL;
        Mb5Track track = mb5_track_list_item(track_list, i);
        Mb5Recording recording = mb5_track_get_recording(track);

        READ_MB(mb5_recording_get_id, recording, tmp_dict, "mbid", 0);

        Mb5ArtistCredit credit;
        if (recording) {
            READ_MB(mb5_recording_get_title, recording, tmp_dict, "title", 0);
            credit = mb5_recording_get_artistcredit(recording);
        } else {
            READ_MB(mb5_track_get_title, track, tmp_dict, "title", 0);
            credit = mb5_track_get_artistcredit(track);
        }
        if (credit)
            mb_credit(credit, tmp_dict, "artist");

        if (dict_get(tmp_dict, "mbid"))
            crc = av_crc(crc_tab, crc, dict_get(tmp_dict, "mbid"), strlen(dict_get(tmp_dict, "mbid")));
        if (dict_get(tmp_dict, "artist"))
            crc = av_crc(crc_tab, crc, dict_get(tmp_dict, "artist"), strlen(dict_get(tmp_dict, "artist")));
        if (dict_get(tmp_dict, "title"))
            crc = av_crc(crc_tab, crc, dict_get(tmp_dict, "title"), strlen(dict_get(tmp_dict, "title")));

        av_dict_free(&tmp_dict);
        crc ^= mb5_track_get_length(track);
        crc ^= i;
    }

    return crc;
}

static int mb_tracks(cyanrip_ctx *ctx, Mb5Release release, const char *discid, int discnumber)
{
    /* Set totaldiscs if possible */
    Mb5MediumList medium_list_extra = NULL;
    Mb5MediumList medium_full_list = mb5_release_get_mediumlist(release);
    int num_cds = mb5_medium_list_size(medium_full_list);
    av_dict_set_int(&ctx->meta, "totaldiscs", num_cds, 0);

    if (num_cds == 1 && !discnumber)
        av_dict_set_int(&ctx->meta, "disc", 1, 0);

    Mb5Medium medium = NULL;
    if (discnumber) {
        if (discnumber < 1 || discnumber > num_cds) {
            cyanrip_log(ctx, 0, "Invalid disc number %i, release only has %i CDs\n", discnumber, num_cds);
            return 1;
        }
        medium = mb5_medium_list_item(medium_full_list, discnumber - 1);
        if (!medium) {
            cyanrip_log(ctx, 0, "Got empty medium list.\n");
            return 1;
        }
    } else {
        medium_list_extra = mb5_release_media_matching_discid(release, discid);
        if (!medium_list_extra) {
            cyanrip_log(ctx, 0, "No mediums match DiscID!\n");
            return 0;
        }

        medium = mb5_medium_list_item(medium_list_extra, 0);
        if (!medium) {
            cyanrip_log(ctx, 0, "Got empty medium list.\n");
            mb5_medium_list_delete(medium_list_extra);
            return 1;
        }

        if (num_cds > 1) {
            uint32_t medium_crc = crc_medium(medium);
            for (int i = 0; i < num_cds; i++) {
                Mb5Medium tmp_medium = mb5_medium_list_item(medium_full_list, i);
                if (medium_crc == crc_medium(tmp_medium)) {
                    av_dict_set_int(&ctx->meta, "disc", i + 1, 0);
                    break;
                }
            }
        }
    }

    READ_MB(mb5_medium_get_title, medium, ctx->meta, "discname", 0);
    READ_MB(mb5_medium_get_format, medium, ctx->meta, "format", 0);

    Mb5TrackList track_list = mb5_medium_get_tracklist(medium);
    if (!track_list) {
        cyanrip_log(ctx, 0, "Medium has no track list.\n");
        if (medium_list_extra)
            mb5_medium_list_delete(medium_list_extra);
        return 0;
    }

    for (int i = 0; i < mb5_track_list_size(track_list); i++) {
        if (i >= ctx->nb_cd_tracks)
            break;
        Mb5Track track = mb5_track_list_item(track_list, i);
        Mb5Recording recording = mb5_track_get_recording(track);

        READ_MB(mb5_recording_get_id, recording, ctx->tracks[i].meta, "mbid", 0);

        Mb5ArtistCredit credit;
        if (recording) {
            READ_MB(mb5_recording_get_title, recording, ctx->tracks[i].meta, "title", 0);
            credit = mb5_recording_get_artistcredit(recording);
        } else {
            READ_MB(mb5_track_get_title, track, ctx->tracks[i].meta, "title", 0);
            credit = mb5_track_get_artistcredit(track);
        }
        if (credit)
            mb_credit(credit, ctx->tracks[i].meta, "artist");
    }

    if (medium_list_extra)
        mb5_medium_list_delete(medium_list_extra);

    return 0;
}

static int mb_metadata(cyanrip_ctx *ctx, int manual_metadata_specified, int release_idx, char *release_str, int discnumber)
{
    int ret = 0, notfound = 0, possible_stub = 0;
    const char *ua = "cyanrip/" PROJECT_VERSION_STRING " ( https://github.com/cyanreg/cyanrip )";
    Mb5Query query = mb5_query_new(ua, NULL, 0);
    if (!query) {
        cyanrip_log(ctx, 0, "Could not connect to MusicBrainz.\n");
        return 1;
    }

    char *names[] = { "inc" };
    char *values[] = { "recordings artist-credits" };
    const char *discid = dict_get(ctx->meta, "discid");
    if (!discid) {
        cyanrip_log(ctx, 0, "Missing DiscID!\n");
        return 0;
    }

    Mb5Metadata metadata = mb5_query_query(query, "discid", discid, 0, 1, names, values);
    if (!metadata) {
        tQueryResult res = mb5_query_get_lastresult(query);
        if (res != eQuery_ResourceNotFound) {
            int chars = mb5_query_get_lasterrormessage(query, NULL, 0) + 1;
            char *msg = av_mallocz(chars*sizeof(*msg));
            mb5_query_get_lasterrormessage(query, msg, chars);
            cyanrip_log(ctx, 0, "MusicBrainz query failed: %s\n", msg);
            av_freep(&msg);
        }

        switch(res) {
        case eQuery_Timeout:
        case eQuery_ConnectionError:
            cyanrip_log(ctx, 0, "Connection failed, try again? Or disable via -N\n");
            break;
        case eQuery_AuthenticationError:
        case eQuery_FetchError:
        case eQuery_RequestError:
            cyanrip_log(ctx, 0, "Error fetching/requesting/auth, this shouldn't happen.\n");
            break;
        case eQuery_ResourceNotFound:
            notfound = 1;
            break;
        default:
            break;
        }

        ret = 1;
        goto end;
    }

    Mb5ReleaseList release_list = NULL;
    Mb5Disc disc = mb5_metadata_get_disc(metadata);
    if (!disc) {
        possible_stub = 1;
        notfound = 1;
        goto end_meta;
    }

    release_list = mb5_disc_get_releaselist(disc);
    if (!release_list) {
        cyanrip_log(ctx, 0, "MusicBrainz lookup failed: DiscID has no associated releases.\n");
        notfound = 1;
        goto end_meta;
    }

    Mb5Release release = NULL;
    int num_releases = mb5_release_list_size(release_list);
    if (!num_releases) {
        cyanrip_log(ctx, 0, "MusicBrainz lookup failed: no releases found for DiscID.\n");
        notfound = 1;
        goto end_meta;
    } else if (num_releases > 1 && ((release_idx < 0) && !release_str)) {
        cyanrip_log(ctx, 0, "Multiple releases found in database for DiscID %s:\n", discid);
        for (int i = 0; i < num_releases; i++) {
            release = mb5_release_list_item(release_list, i);
            AVDictionary *tmp_dict = NULL;
            READ_MB(mb5_release_get_date, release, tmp_dict, "date", 0);
            READ_MB(mb5_release_get_title, release, tmp_dict, "album", 0);
            READ_MB(mb5_release_get_id, release, tmp_dict, "id", 0);
            READ_MB(mb5_release_get_disambiguation, release, tmp_dict, "disambiguation", 0);
            READ_MB(mb5_release_get_country, release, tmp_dict, "country", 0);

            Mb5MediumList medium_list = mb5_release_get_mediumlist(release);
            int num_cds = mb5_medium_list_size(medium_list);
            if (num_cds > 1)
                av_dict_set_int(&tmp_dict, "num_cds", num_cds, 0);

#define PROP(key, postamble)                                    \
    (!!dict_get(tmp_dict, key)) ? " (" : "",                    \
    (!!dict_get(tmp_dict, key)) ? dict_get(tmp_dict, key) : "", \
    (!!dict_get(tmp_dict, key)) ? postamble : "",               \
    (!!dict_get(tmp_dict, key)) ? ")" : ""

            cyanrip_log(ctx, 0, "    %i (ID: %s): %s" "%s%s%s%s" "%s%s%s%s" "%s%s%s%s" "%s%s%s%s" "%s", i + 1,
                        dict_get(tmp_dict, "id")    ? dict_get(tmp_dict, "id")    : "unknown id",
                        dict_get(tmp_dict, "album") ? dict_get(tmp_dict, "album") : "unknown album",
                        PROP("disambiguation", ""),
                        PROP("country", ""),
                        PROP("num_cds", " CDs"),
                        PROP("date", ""),
                        "\n");

#undef PROP

            av_dict_free(&tmp_dict);
        }
        cyanrip_log(ctx, 0, "\n");
        cyanrip_log(ctx, 0, "Please specify which release to use by adding the -R argument with an index or ID.\n");
        ret = 1;
        goto end_meta;
    } else if (release_idx >= 0) { /* Release index specified */
        if ((release_idx < 1) || (release_idx > num_releases)) {
            cyanrip_log(ctx, 0, "Invalid release index %i specified, only have %i releases!\n", release_idx, num_releases);
            ret = 1;
            goto end_meta;
        }
        release = mb5_release_list_item(release_list, release_idx - 1);
    } else if (release_str) { /* Release ID specified */
        int i = 0;
        for (; i < num_releases; i++) {
            release = mb5_release_list_item(release_list, i);
            AVDictionary *tmp_dict = NULL;
            READ_MB(mb5_release_get_id, release, tmp_dict, "id", 0);
            if (dict_get(tmp_dict, "id") && !strcmp(release_str, dict_get(tmp_dict, "id"))) {
                av_dict_free(&tmp_dict);
                break;
            }
            av_dict_free(&tmp_dict);
        }
        if (i == num_releases) {
            cyanrip_log(ctx, 0, "Release ID %s not found in release list for DiscID %s!\n", release_str, discid);
            ret = 1;
            goto end_meta;
        }
    } else {
        release = mb5_release_list_item(release_list, 0);
    }

    READ_MB(mb5_release_get_id, release, ctx->meta, "musicbrainz_albumid", 0);
    READ_MB(mb5_release_get_disambiguation, release, ctx->meta, "releasecomment", 0);
    READ_MB(mb5_release_get_date, release, ctx->meta, "date", 0);
    READ_MB(mb5_release_get_title, release, ctx->meta, "album", 0);
    READ_MB(mb5_release_get_barcode, release, ctx->meta, "barcode", 0);
    READ_MB(mb5_release_get_packaging, release, ctx->meta, "packaging", 0);
    READ_MB(mb5_release_get_country, release, ctx->meta, "country", 0);
    READ_MB(mb5_release_get_status, release, ctx->meta, "status", 0);

    /* Label info */
    Mb5LabelInfoList *labelinfolist = mb5_release_get_labelinfolist(release);
    if (mb5_labelinfo_list_size(labelinfolist) == 1) {
        Mb5LabelInfo *labelinfo = mb5_label_list_item(labelinfolist, 0);
        READ_MB(mb5_labelinfo_get_catalognumber, labelinfo, ctx->meta, "catalog", 0);

        Mb5Label *label = mb5_labelinfo_get_label(labelinfo);
        READ_MB(mb5_label_get_name, label, ctx->meta, "label", 0);
    }

    Mb5ArtistCredit artistcredit = mb5_release_get_artistcredit(release);
    if (artistcredit)
        mb_credit(artistcredit, ctx->meta, "album_artist");

    cyanrip_log(ctx, 0, "Found MusicBrainz release: %s - %s\n",
                dict_get(ctx->meta, "album"), dict_get(ctx->meta, "album_artist"));

    /* Read track metadata */
    mb_tracks(ctx, release, discid, discnumber);

end_meta:
    mb5_metadata_delete(metadata); /* This frees _all_ metadata */

end:
    mb5_query_delete(query);

    if (notfound) {
        if (possible_stub) {
            cyanrip_log(ctx, 0, "MusicBrainz lookup failed, but DiscID has a matching stub, "
                        "consider verifying the data and creating a release here:\n");
            ret = 1;
        } else if (!manual_metadata_specified) {
            cyanrip_log(ctx, 0, "Unable to find release info for this CD, "
                        "and metadata hasn't been manually added!\n");
            ret = 1;
        } else {
            cyanrip_log(ctx, 0, "Unable to find metadata for this CD, but "
                        "metadata has been manually specified, continuing.\n");
            ret = 0;
        }

        if (!possible_stub) {
            cyanrip_log(ctx, 0, "Please help improve the MusicBrainz DB by "
                        "submitting the disc info via the following URL:\n");
        }

        cyanrip_log(ctx, 0, "%s\n", ctx->mb_submission_url);
        if (ret)
            cyanrip_log(ctx, 0, "To continue add metadata via -a or -t, or ignore via -N!\n");
    }

    return ret;
}

int crip_fill_metadata(cyanrip_ctx *ctx, int manual_metadata_specified,
                       int release_idx, char *release_str, int discnumber)
{
    /* Get musicbrainz tags */
    if (!ctx->settings.disable_mb)
        return mb_metadata(ctx, manual_metadata_specified, release_idx, release_str, discnumber);

    return 0;
}
