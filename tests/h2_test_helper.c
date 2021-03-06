/*
 * Copyright 2010-2018 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "h2_test_helper.h"

#include <aws/http/private/h2_decoder.h>
#include <aws/io/stream.h>
#include <aws/testing/io_testing_channel.h>

/*******************************************************************************
 * h2_decoded_frame
 ******************************************************************************/
static int s_frame_init(
    struct h2_decoded_frame *frame,
    struct aws_allocator *alloc,
    enum aws_h2_frame_type type,
    uint32_t stream_id) {

    AWS_ZERO_STRUCT(*frame);
    frame->type = type;
    frame->stream_id = stream_id;
    frame->headers = aws_http_headers_new(alloc);
    ASSERT_SUCCESS(aws_array_list_init_dynamic(&frame->settings, alloc, 16, sizeof(struct aws_h2_frame_setting)));
    ASSERT_SUCCESS(aws_byte_buf_init(&frame->data, alloc, 1024));
    return AWS_OP_SUCCESS;
}

static void s_frame_clean_up(struct h2_decoded_frame *frame) {
    aws_http_headers_release(frame->headers);
    aws_array_list_clean_up(&frame->settings);
    aws_byte_buf_clean_up(&frame->data);
}

int h2_decoded_frame_check_finished(
    const struct h2_decoded_frame *frame,
    enum aws_h2_frame_type expected_type,
    uint32_t expected_stream_id) {

    ASSERT_INT_EQUALS(expected_type, frame->type);
    ASSERT_UINT_EQUALS(expected_stream_id, frame->stream_id);
    ASSERT_TRUE(frame->finished);
    return AWS_OP_SUCCESS;
}

/*******************************************************************************
 * h2_decode_tester
 ******************************************************************************/

size_t h2_decode_tester_frame_count(const struct h2_decode_tester *decode_tester) {
    return aws_array_list_length(&decode_tester->frames);
}

struct h2_decoded_frame *h2_decode_tester_get_frame(const struct h2_decode_tester *decode_tester, size_t i) {
    AWS_FATAL_ASSERT(h2_decode_tester_frame_count(decode_tester) > i);
    struct h2_decoded_frame *frame = NULL;
    aws_array_list_get_at_ptr(&decode_tester->frames, (void **)&frame, i);
    return frame;
}

struct h2_decoded_frame *h2_decode_tester_latest_frame(const struct h2_decode_tester *decode_tester) {
    size_t frame_count = h2_decode_tester_frame_count(decode_tester);
    AWS_FATAL_ASSERT(frame_count != 0);
    return h2_decode_tester_get_frame(decode_tester, frame_count - 1);
}

int h2_decode_tester_check_data_across_frames(
    const struct h2_decode_tester *decode_tester,
    uint32_t stream_id,
    struct aws_byte_cursor expected,
    bool expect_end_stream) {

    struct aws_byte_buf data;
    ASSERT_SUCCESS(aws_byte_buf_init(&data, decode_tester->alloc, 128));

    bool found_end_stream = false;

    for (size_t frame_i = 0; frame_i < h2_decode_tester_frame_count(decode_tester); ++frame_i) {
        struct h2_decoded_frame *frame = h2_decode_tester_get_frame(decode_tester, frame_i);

        if (frame->type == AWS_H2_FRAME_T_DATA && frame->stream_id == stream_id) {
            struct aws_byte_cursor frame_data = aws_byte_cursor_from_buf(&frame->data);
            ASSERT_SUCCESS(aws_byte_buf_append_dynamic(&data, &frame_data));

            found_end_stream = frame->end_stream;
        }
    }

    ASSERT_BIN_ARRAYS_EQUALS(expected.ptr, expected.len, data.buffer, data.len);
    ASSERT_UINT_EQUALS(expect_end_stream, found_end_stream);

    aws_byte_buf_clean_up(&data);
    return AWS_OP_SUCCESS;
}

int h2_decode_tester_check_data_str_across_frames(
    const struct h2_decode_tester *decode_tester,
    uint32_t stream_id,
    const char *expected,
    bool expect_end_stream) {

    return h2_decode_tester_check_data_across_frames(
        decode_tester, stream_id, aws_byte_cursor_from_c_str(expected), expect_end_stream);
}

/* decode-tester begins recording a new frame's data */
static int s_begin_new_frame(
    struct h2_decode_tester *decode_tester,
    enum aws_h2_frame_type type,
    uint32_t stream_id,
    struct h2_decoded_frame **out_frame) {

    /* If there's a previous frame, assert that we know it was finished.
     * If this fails, some on_X_begin(), on_X_i(), on_X_end() loop didn't fire correctly.
     * It should be impossible for an unrelated callback to fire during these loops */
    if (aws_array_list_length(&decode_tester->frames) > 0) {
        const struct h2_decoded_frame *prev_frame = h2_decode_tester_latest_frame(decode_tester);
        ASSERT_TRUE(prev_frame->finished);
    }

    /* Create new frame */
    struct h2_decoded_frame new_frame;
    ASSERT_SUCCESS(s_frame_init(&new_frame, decode_tester->alloc, type, stream_id));
    ASSERT_SUCCESS(aws_array_list_push_back(&decode_tester->frames, &new_frame));

    if (out_frame) {
        aws_array_list_get_at_ptr(
            &decode_tester->frames, (void **)out_frame, aws_array_list_length(&decode_tester->frames) - 1);
    }
    return AWS_OP_SUCCESS;
}

/* decode-tester stops recording the latest frame's data */
static int s_end_current_frame(
    struct h2_decode_tester *decode_tester,
    enum aws_h2_frame_type type,
    uint32_t stream_id) {
    struct h2_decoded_frame *frame = h2_decode_tester_latest_frame(decode_tester);
    ASSERT_FALSE(frame->finished);
    frame->finished = true;
    ASSERT_SUCCESS(h2_decoded_frame_check_finished(frame, type, stream_id));
    return AWS_OP_SUCCESS;
}

static int s_decoder_on_headers_begin(uint32_t stream_id, void *userdata) {
    struct h2_decode_tester *decode_tester = userdata;
    ASSERT_SUCCESS(s_begin_new_frame(decode_tester, AWS_H2_FRAME_T_HEADERS, stream_id, NULL /*out_frame*/));
    return AWS_OP_SUCCESS;
}

static int s_on_header(
    bool is_push_promise,
    uint32_t stream_id,
    const struct aws_http_header *header,
    enum aws_http_header_name name_enum,
    enum aws_http_header_block block_type,
    void *userdata) {

    struct h2_decode_tester *decode_tester = userdata;
    struct h2_decoded_frame *frame = h2_decode_tester_latest_frame(decode_tester);

    /* Validate */
    if (is_push_promise) {
        ASSERT_INT_EQUALS(AWS_H2_FRAME_T_PUSH_PROMISE, frame->type);
    } else {
        ASSERT_INT_EQUALS(AWS_H2_FRAME_T_HEADERS, frame->type);

        /* block-type should be same for each header in block */
        if (aws_http_headers_count(frame->headers) > 0) {
            ASSERT_INT_EQUALS(frame->header_block_type, block_type);
        }
    }

    ASSERT_FALSE(frame->finished);
    ASSERT_UINT_EQUALS(frame->stream_id, stream_id);
    ASSERT_INT_EQUALS(aws_http_lowercase_str_to_header_name(header->name), name_enum);

    /* Stash header */
    ASSERT_SUCCESS(aws_http_headers_add_header(frame->headers, header));
    frame->header_block_type = block_type;

    return AWS_OP_SUCCESS;
}

static int s_decoder_on_headers_i(
    uint32_t stream_id,
    const struct aws_http_header *header,
    enum aws_http_header_name name_enum,
    enum aws_http_header_block block_type,
    void *userdata) {
    return s_on_header(false /* is_push_promise */, stream_id, header, name_enum, block_type, userdata);
}

static int s_on_headers_end(
    bool is_push_promise,
    uint32_t stream_id,
    bool malformed,
    enum aws_http_header_block block_type,
    void *userdata) {

    struct h2_decode_tester *decode_tester = userdata;
    struct h2_decoded_frame *frame = h2_decode_tester_latest_frame(decode_tester);

    /* end() should report same block-type as i() calls */
    if (!is_push_promise && aws_http_headers_count(frame->headers) > 0) {
        ASSERT_INT_EQUALS(frame->header_block_type, block_type);
    }
    frame->header_block_type = block_type;

    frame->headers_malformed = malformed;
    ASSERT_SUCCESS(s_end_current_frame(
        decode_tester, is_push_promise ? AWS_H2_FRAME_T_PUSH_PROMISE : AWS_H2_FRAME_T_HEADERS, stream_id));
    return AWS_OP_SUCCESS;
}

static int s_decoder_on_headers_end(
    uint32_t stream_id,
    bool malformed,
    enum aws_http_header_block block_type,
    void *userdata) {

    return s_on_headers_end(false /*is_push_promise*/, stream_id, malformed, block_type, userdata);
}

static int s_decoder_on_push_promise_begin(uint32_t stream_id, uint32_t promised_stream_id, void *userdata) {
    struct h2_decode_tester *decode_tester = userdata;
    struct h2_decoded_frame *frame;
    ASSERT_SUCCESS(s_begin_new_frame(decode_tester, AWS_H2_FRAME_T_PUSH_PROMISE, stream_id, &frame /*out_frame*/));

    frame->promised_stream_id = promised_stream_id;

    return AWS_OP_SUCCESS;
}

static int s_decoder_on_push_promise_i(
    uint32_t stream_id,
    const struct aws_http_header *header,
    enum aws_http_header_name name_enum,
    void *userdata) {
    return s_on_header(true /* is_push_promise */, stream_id, header, name_enum, AWS_HTTP_HEADER_BLOCK_MAIN, userdata);
}

static int s_decoder_on_push_promise_end(uint32_t stream_id, bool malformed, void *userdata) {
    return s_on_headers_end(true /*is_push_promise*/, stream_id, malformed, AWS_HTTP_HEADER_BLOCK_MAIN, userdata);
}

static int s_decoder_on_data(uint32_t stream_id, struct aws_byte_cursor data, void *userdata) {
    struct h2_decode_tester *decode_tester = userdata;
    struct h2_decoded_frame *frame;

    /* Pretend each on_data callback is a full DATA frame for the purposes of these tests */
    ASSERT_SUCCESS(s_begin_new_frame(decode_tester, AWS_H2_FRAME_T_DATA, stream_id, &frame));

    /* Stash data*/
    ASSERT_SUCCESS(aws_byte_buf_append_dynamic(&frame->data, &data));

    ASSERT_SUCCESS(s_end_current_frame(decode_tester, AWS_H2_FRAME_T_DATA, stream_id));
    return AWS_OP_SUCCESS;
}

static int s_decoder_on_end_stream(uint32_t stream_id, void *userdata) {
    struct h2_decode_tester *decode_tester = userdata;
    struct h2_decoded_frame *frame = h2_decode_tester_latest_frame(decode_tester);

    /* Validate */

    /* on_end_stream should fire IMMEDIATELY after on_data OR after on_headers_end.
     * This timing lets the user close the stream from this callback without waiting for any trailing data/headers
     */
    ASSERT_TRUE(frame->finished);
    ASSERT_TRUE(frame->type == AWS_H2_FRAME_T_HEADERS || frame->type == AWS_H2_FRAME_T_DATA);
    ASSERT_UINT_EQUALS(frame->stream_id, stream_id);

    ASSERT_FALSE(frame->end_stream);

    /* Stash */
    frame->end_stream = true;

    return AWS_OP_SUCCESS;
}

static int s_decoder_on_rst_stream(uint32_t stream_id, uint32_t error_code, void *userdata) {
    struct h2_decode_tester *decode_tester = userdata;
    struct h2_decoded_frame *frame;

    ASSERT_SUCCESS(s_begin_new_frame(decode_tester, AWS_H2_FRAME_T_RST_STREAM, stream_id, &frame));

    /* Stash data*/
    frame->error_code = error_code;

    ASSERT_SUCCESS(s_end_current_frame(decode_tester, AWS_H2_FRAME_T_RST_STREAM, stream_id));
    return AWS_OP_SUCCESS;
}

static int s_decoder_on_settings(
    const struct aws_h2_frame_setting *settings_array,
    size_t num_settings,
    void *userdata) {
    struct h2_decode_tester *decode_tester = userdata;
    struct h2_decoded_frame *frame;
    ASSERT_SUCCESS(s_begin_new_frame(decode_tester, AWS_H2_FRAME_T_SETTINGS, 0, &frame));

    /* Stash setting */
    for (size_t i = 0; i < num_settings; i++) {
        ASSERT_SUCCESS(aws_array_list_push_back(&frame->settings, &settings_array[i]));
    }

    ASSERT_SUCCESS(s_end_current_frame(decode_tester, AWS_H2_FRAME_T_SETTINGS, 0));
    return AWS_OP_SUCCESS;
}

static int s_decoder_on_settings_ack(void *userdata) {
    struct h2_decode_tester *decode_tester = userdata;
    struct h2_decoded_frame *frame;

    ASSERT_SUCCESS(s_begin_new_frame(decode_tester, AWS_H2_FRAME_T_SETTINGS, 0 /*stream_id*/, &frame));

    /* Stash data*/
    frame->ack = true;

    ASSERT_SUCCESS(s_end_current_frame(decode_tester, AWS_H2_FRAME_T_SETTINGS, 0 /*stream_id*/));
    return AWS_OP_SUCCESS;
}

static int s_decoder_on_ping(uint8_t opaque_data[AWS_H2_PING_DATA_SIZE], void *userdata) {
    struct h2_decode_tester *decode_tester = userdata;
    struct h2_decoded_frame *frame;

    ASSERT_SUCCESS(s_begin_new_frame(decode_tester, AWS_H2_FRAME_T_PING, 0 /*stream_id*/, &frame));

    /* Stash data*/
    memcpy(frame->ping_opaque_data, opaque_data, AWS_H2_PING_DATA_SIZE);

    ASSERT_SUCCESS(s_end_current_frame(decode_tester, AWS_H2_FRAME_T_PING, 0 /*stream_id*/));
    return AWS_OP_SUCCESS;
}

static int s_decoder_on_ping_ack(uint8_t opaque_data[AWS_H2_PING_DATA_SIZE], void *userdata) {
    struct h2_decode_tester *decode_tester = userdata;
    struct h2_decoded_frame *frame;

    ASSERT_SUCCESS(s_begin_new_frame(decode_tester, AWS_H2_FRAME_T_PING, 0 /*stream_id*/, &frame));

    /* Stash data*/
    memcpy(frame->ping_opaque_data, opaque_data, AWS_H2_PING_DATA_SIZE);
    frame->ack = true;

    ASSERT_SUCCESS(s_end_current_frame(decode_tester, AWS_H2_FRAME_T_PING, 0 /*stream_id*/));
    return AWS_OP_SUCCESS;
}

static int s_decoder_on_goaway_begin(
    uint32_t last_stream,
    uint32_t error_code,
    uint32_t debug_data_length,
    void *userdata) {

    struct h2_decode_tester *decode_tester = userdata;
    struct h2_decoded_frame *frame;
    ASSERT_SUCCESS(s_begin_new_frame(decode_tester, AWS_H2_FRAME_T_GOAWAY, 0, &frame));

    frame->goaway_last_stream_id = last_stream;
    frame->error_code = error_code;
    frame->goaway_debug_data_remaining = debug_data_length;

    return AWS_OP_SUCCESS;
}

static int s_decoder_on_goaway_i(struct aws_byte_cursor debug_data, void *userdata) {
    struct h2_decode_tester *decode_tester = userdata;
    struct h2_decoded_frame *frame = h2_decode_tester_latest_frame(decode_tester);

    /* Validate */
    ASSERT_INT_EQUALS(AWS_H2_FRAME_T_GOAWAY, frame->type);
    ASSERT_FALSE(frame->finished);
    ASSERT_TRUE(frame->goaway_debug_data_remaining >= debug_data.len);

    frame->goaway_debug_data_remaining -= (uint32_t)debug_data.len;

    /* Stash data */
    ASSERT_SUCCESS(aws_byte_buf_append_dynamic(&frame->data, &debug_data));

    return AWS_OP_SUCCESS;
}

static int s_decoder_on_goaway_end(void *userdata) {
    struct h2_decode_tester *decode_tester = userdata;
    ASSERT_SUCCESS(s_end_current_frame(decode_tester, AWS_H2_FRAME_T_GOAWAY, 0));

    struct h2_decoded_frame *frame = h2_decode_tester_latest_frame(decode_tester);
    ASSERT_UINT_EQUALS(0, frame->goaway_debug_data_remaining);

    return AWS_OP_SUCCESS;
}

static int s_decoder_on_window_update(uint32_t stream_id, uint32_t window_size_increment, void *userdata) {
    struct h2_decode_tester *decode_tester = userdata;
    struct h2_decoded_frame *frame;
    ASSERT_SUCCESS(s_begin_new_frame(decode_tester, AWS_H2_FRAME_T_WINDOW_UPDATE, stream_id, &frame));

    frame->window_size_increment = window_size_increment;

    ASSERT_SUCCESS(s_end_current_frame(decode_tester, AWS_H2_FRAME_T_WINDOW_UPDATE, stream_id));

    return AWS_OP_SUCCESS;
}

static struct aws_h2_decoder_vtable s_decoder_vtable = {
    .on_headers_begin = s_decoder_on_headers_begin,
    .on_headers_i = s_decoder_on_headers_i,
    .on_headers_end = s_decoder_on_headers_end,
    .on_push_promise_begin = s_decoder_on_push_promise_begin,
    .on_push_promise_i = s_decoder_on_push_promise_i,
    .on_push_promise_end = s_decoder_on_push_promise_end,
    .on_data = s_decoder_on_data,
    .on_end_stream = s_decoder_on_end_stream,
    .on_rst_stream = s_decoder_on_rst_stream,
    .on_settings = s_decoder_on_settings,
    .on_settings_ack = s_decoder_on_settings_ack,
    .on_ping = s_decoder_on_ping,
    .on_ping_ack = s_decoder_on_ping_ack,
    .on_goaway_begin = s_decoder_on_goaway_begin,
    .on_goaway_i = s_decoder_on_goaway_i,
    .on_goaway_end = s_decoder_on_goaway_end,
    .on_window_update = s_decoder_on_window_update,
};

int h2_decode_tester_init(struct h2_decode_tester *decode_tester, const struct h2_decode_tester_options *options) {
    AWS_ZERO_STRUCT(*decode_tester);
    decode_tester->alloc = options->alloc;

    struct aws_h2_decoder_params decoder_params = {
        .alloc = options->alloc,
        .vtable = &s_decoder_vtable,
        .userdata = decode_tester,
        .is_server = options->is_server,
        .skip_connection_preface = options->skip_connection_preface,
    };
    decode_tester->decoder = aws_h2_decoder_new(&decoder_params);
    ASSERT_NOT_NULL(decode_tester->decoder);

    ASSERT_SUCCESS(
        aws_array_list_init_dynamic(&decode_tester->frames, options->alloc, 16, sizeof(struct h2_decoded_frame)));
    return AWS_OP_SUCCESS;
}

void h2_decode_tester_clean_up(struct h2_decode_tester *decode_tester) {
    aws_h2_decoder_destroy(decode_tester->decoder);

    for (size_t i = 0; i < aws_array_list_length(&decode_tester->frames); ++i) {
        struct h2_decoded_frame *frame;
        aws_array_list_get_at_ptr(&decode_tester->frames, (void **)&frame, i);
        s_frame_clean_up(frame);
    }
    aws_array_list_clean_up(&decode_tester->frames);

    AWS_ZERO_STRUCT(*decode_tester);
}

/*******************************************************************************
 * h2_fake_peer
 ******************************************************************************/

int h2_fake_peer_init(struct h2_fake_peer *peer, const struct h2_fake_peer_options *options) {
    AWS_ZERO_STRUCT(*peer);
    peer->alloc = options->alloc;
    peer->testing_channel = options->testing_channel;
    peer->is_server = options->is_server;

    ASSERT_SUCCESS(aws_h2_frame_encoder_init(&peer->encoder, peer->alloc, NULL /*logging_id*/));

    struct h2_decode_tester_options decode_options = {.alloc = options->alloc, .is_server = options->is_server};
    ASSERT_SUCCESS(h2_decode_tester_init(&peer->decode, &decode_options));
    return AWS_OP_SUCCESS;
}

void h2_fake_peer_clean_up(struct h2_fake_peer *peer) {
    aws_h2_frame_encoder_clean_up(&peer->encoder);
    h2_decode_tester_clean_up(&peer->decode);
    AWS_ZERO_STRUCT(peer);
}

int h2_fake_peer_decode_messages_from_testing_channel(struct h2_fake_peer *peer) {
    struct aws_byte_buf msg_buf;
    ASSERT_SUCCESS(aws_byte_buf_init(&msg_buf, peer->alloc, 128));
    ASSERT_SUCCESS(testing_channel_drain_written_messages(peer->testing_channel, &msg_buf));

    struct aws_byte_cursor msg_cursor = aws_byte_cursor_from_buf(&msg_buf);
    ASSERT_SUCCESS(aws_h2_decode(peer->decode.decoder, &msg_cursor));
    ASSERT_UINT_EQUALS(0, msg_cursor.len);

    aws_byte_buf_clean_up(&msg_buf);
    return AWS_OP_SUCCESS;
}

int h2_fake_peer_send_frame(struct h2_fake_peer *peer, struct aws_h2_frame *frame) {
    ASSERT_NOT_NULL(frame);

    bool frame_complete = false;
    while (!frame_complete) {
        struct aws_io_message *msg = aws_channel_acquire_message_from_pool(
            peer->testing_channel->channel, AWS_IO_MESSAGE_APPLICATION_DATA, g_aws_channel_max_fragment_size);
        ASSERT_NOT_NULL(msg);

        ASSERT_SUCCESS(aws_h2_encode_frame(&peer->encoder, frame, &msg->message_data, &frame_complete));
        ASSERT_TRUE(msg->message_data.len != 0);

        ASSERT_SUCCESS(testing_channel_push_read_message(peer->testing_channel, msg));
    }

    aws_h2_frame_destroy(frame);
    return AWS_OP_SUCCESS;
}

int h2_fake_peer_send_data_frame(
    struct h2_fake_peer *peer,
    uint32_t stream_id,
    struct aws_byte_cursor data,
    bool end_stream) {

    struct aws_input_stream *body_stream = aws_input_stream_new_from_cursor(peer->alloc, &data);
    ASSERT_NOT_NULL(body_stream);

    struct aws_io_message *msg = aws_channel_acquire_message_from_pool(
        peer->testing_channel->channel, AWS_IO_MESSAGE_APPLICATION_DATA, g_aws_channel_max_fragment_size);
    ASSERT_NOT_NULL(msg);

    bool body_complete;
    ASSERT_SUCCESS(aws_h2_encode_data_frame(
        &peer->encoder, stream_id, body_stream, end_stream, 0, &msg->message_data, &body_complete));

    ASSERT_TRUE(body_complete);
    ASSERT_TRUE(msg->message_data.len != 0);

    ASSERT_SUCCESS(testing_channel_push_read_message(peer->testing_channel, msg));
    aws_input_stream_destroy(body_stream);
    return AWS_OP_SUCCESS;
}

int h2_fake_peer_send_data_frame_str(struct h2_fake_peer *peer, uint32_t stream_id, const char *data, bool end_stream) {
    return h2_fake_peer_send_data_frame(peer, stream_id, aws_byte_cursor_from_c_str(data), end_stream);
}

int h2_fake_peer_send_connection_preface(struct h2_fake_peer *peer, struct aws_h2_frame *settings) {
    if (!peer->is_server) {
        /* Client must first send magic string */
        ASSERT_SUCCESS(testing_channel_push_read_data(peer->testing_channel, aws_h2_connection_preface_client_string));
    }

    /* Both server and client send SETTINGS as first proper frame */
    ASSERT_SUCCESS(h2_fake_peer_send_frame(peer, settings));

    return AWS_OP_SUCCESS;
}

int h2_fake_peer_send_connection_preface_default_settings(struct h2_fake_peer *peer) {
    /* Empty SETTINGS frame means "everything default" */
    struct aws_h2_frame *settings = aws_h2_frame_new_settings(peer->alloc, NULL, 0, false /*ack*/);
    ASSERT_NOT_NULL(settings);

    ASSERT_SUCCESS(h2_fake_peer_send_connection_preface(peer, settings));
    return AWS_OP_SUCCESS;
}
