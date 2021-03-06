#ifndef AWS_HTTP_H2_STREAM_H
#define AWS_HTTP_H2_STREAM_H

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

#include <aws/http/private/h2_frames.h>
#include <aws/http/private/request_response_impl.h>

#include <aws/common/mutex.h>

#include <inttypes.h>

#define AWS_H2_STREAM_LOGF(level, stream, text, ...)                                                                   \
    AWS_LOGF_##level(                                                                                                  \
        AWS_LS_HTTP_STREAM,                                                                                            \
        "id=%" PRIu32 " connection=%p state=%s: " text,                                                                \
        (stream)->base.id,                                                                                             \
        (void *)(stream)->base.owning_connection,                                                                      \
        aws_h2_stream_state_to_str((stream)->thread_data.state),                                                       \
        __VA_ARGS__)
#define AWS_H2_STREAM_LOG(level, stream, text) AWS_H2_STREAM_LOGF(level, (stream), "%s", (text))

enum aws_h2_stream_state {
    /* Initial state, before anything sent or received. */
    AWS_H2_STREAM_STATE_IDLE,
    /* (server-only) stream-id was reserved via PUSH_PROMISE on another stream,
     * but HEADERS for this stream have not been sent yet */
    AWS_H2_STREAM_STATE_RESERVED_LOCAL,
    /* (client-only) stream-id was reserved via PUSH_PROMISE on another stream,
     * but HEADERS for this stream have not been received yet */
    AWS_H2_STREAM_STATE_RESERVED_REMOTE,
    /* Neither side is done sending their message. */
    AWS_H2_STREAM_STATE_OPEN,
    /* This side is done sending message (END_STREAM), but peer is not done. */
    AWS_H2_STREAM_STATE_HALF_CLOSED_LOCAL,
    /* Peer is done sending message (END_STREAM), but this side is not done */
    AWS_H2_STREAM_STATE_HALF_CLOSED_REMOTE,
    /* Both sides done sending message (END_STREAM),
     * or either side has sent RST_STREAM */
    AWS_H2_STREAM_STATE_CLOSED,

    AWS_H2_STREAM_STATE_COUNT,
};

struct aws_h2_stream {
    struct aws_http_stream base;

    struct aws_linked_list_node node;

    /* Only the event-loop thread may touch this data */
    struct {
        enum aws_h2_stream_state state;
        uint64_t window_size; /* #TODO try to figure out how this actually works, and then implement it */
        struct aws_http_message *outgoing_message;
        bool received_main_headers;
    } thread_data;
};

const char *aws_h2_stream_state_to_str(enum aws_h2_stream_state state);

struct aws_h2_stream *aws_h2_stream_new_request(
    struct aws_http_connection *client_connection,
    const struct aws_http_make_request_options *options);

enum aws_h2_stream_state aws_h2_stream_get_state(const struct aws_h2_stream *stream);

/* Connection is ready to send frames from stream now */
int aws_h2_stream_on_activated(struct aws_h2_stream *stream, bool *out_has_outgoing_data);

int aws_h2_stream_on_decoder_headers_begin(struct aws_h2_stream *stream);

int aws_h2_stream_on_decoder_headers_i(
    struct aws_h2_stream *stream,
    const struct aws_http_header *header,
    enum aws_http_header_name name_enum,
    enum aws_http_header_block block_type);

int aws_h2_stream_on_decoder_headers_end(
    struct aws_h2_stream *stream,
    bool malformed,
    enum aws_http_header_block block_type);

int aws_h2_stream_on_decoder_data(struct aws_h2_stream *stream, struct aws_byte_cursor data);
int aws_h2_stream_on_decoder_end_stream(struct aws_h2_stream *stream);

int aws_h2_stream_activate(struct aws_http_stream *stream);

#endif /* AWS_HTTP_H2_STREAM_H */
