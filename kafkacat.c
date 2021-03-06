/*
 * kafkacat - Apache Kafka consumer and producer
 *
 * Copyright (c) 2014, Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>


#include "kafkacat.h"


struct conf conf = {
        .run = 1,
        .verbosity = 1,
        .partition = RD_KAFKA_PARTITION_UA,
        .msg_size = 1024*1024,
        .null_str = "NULL",
};

static struct stats {
        uint64_t tx;
        uint64_t tx_err_q;
        uint64_t tx_err_dr;
        uint64_t tx_delivered;

        uint64_t rx;
} stats;


/* Partition's at EOF state array */
int *part_eof = NULL;
/* Number of partitions that has reached EOF */
int part_eof_cnt = 0;
/* Threshold level (partitions at EOF) before exiting */
int part_eof_thres = 0;



/**
 * Fatal error: print error and exit
 */
void __attribute__((noreturn)) fatal0 (const char *func, int line,
                                       const char *fmt, ...) {
        va_list ap;
        char buf[1024];

        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);

        INFO(2, "Fatal error at %s:%i:\n", func, line);
        fprintf(stderr, "%% ERROR: %s\n", buf);
        exit(1);
}



/**
 * The delivery report callback is called once per message to
 * report delivery success or failure.
 */
static void dr_msg_cb (rd_kafka_t *rk, const rd_kafka_message_t *rkmessage,
                       void *opaque) {
        static int say_once = 1;

        if (rkmessage->err) {
                INFO(1, "Delivery failed for message: %s\n",
                     rd_kafka_err2str(rkmessage->err));
                stats.tx_err_dr++;
                return;
        }

        INFO(3, "Message delivered to partition %"PRId32" (offset %"PRId64")\n",
             rkmessage->partition, rkmessage->offset);

        if (rkmessage->offset == 0 && say_once) {
                INFO(3, "Enable message offset reporting "
                     "with '-X topic.produce.offset.report=true'\n");
                say_once = 0;
        }
        stats.tx_delivered++;
}


/**
 * Produces a single message, retries on queue congestion, and
 * exits hard on error.
 */
static void produce (void *buf, size_t len,
                     const void *key, size_t key_len, int msgflags) {

        /* Produce message: keep trying until it succeeds. */
        do {
                rd_kafka_resp_err_t err;

                if (!conf.run)
                        FATAL("Program terminated while "
                              "producing message of %zd bytes", len);

                if (rd_kafka_produce(conf.rkt, conf.partition, msgflags,
                                     buf, len, key, key_len, NULL) != -1) {
                        stats.tx++;
                        break;
                }

                err = rd_kafka_errno2err(errno);

                if (err != RD_KAFKA_RESP_ERR__QUEUE_FULL)
                        FATAL("Failed to produce message (%zd bytes): %s",
                              len, rd_kafka_err2str(err));

                stats.tx_err_q++;

                /* Internal queue full, sleep to allow
                 * messages to be produced/time out
                 * before trying again. */
                rd_kafka_poll(conf.rk, 5);
        } while (1);

        /* Poll for delivery reports, errors, etc. */
        rd_kafka_poll(conf.rk, 0);
}


/**
 * Produce contents of file as a single message.
 * Returns the file length on success, else -1.
 */
static ssize_t produce_file (const char *path) {
        int fd;
        void *ptr;
        struct stat st;

        if ((fd = open(path, O_RDONLY)) == -1) {
                INFO(1, "Failed to open %s: %s\n", path, strerror(errno));
                return -1;
        }

        if (fstat(fd, &st) == -1) {
                INFO(1, "Failed to stat %s: %s\n", path, strerror(errno));
                close(fd);
                return -1;
        }

        if (st.st_size == 0) {
                INFO(3, "Skipping empty file %s\n", path);
                close(fd);
                return 0;
        }

        ptr = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (ptr == MAP_FAILED) {
                INFO(1, "Failed to mmap %s: %s\n", path, strerror(errno));
                close(fd);
                return -1;
        }

        INFO(4, "Producing file %s (%"PRIdMAX" bytes)\n",
             path, (intmax_t)st.st_size);
        produce(ptr, st.st_size, NULL, 0, RD_KAFKA_MSG_F_COPY);

        munmap(ptr, st.st_size);
        return st.st_size;
}


/**
 * Run producer, reading messages from 'fp' and producing to kafka.
 * Or if 'pathcnt' is > 0, read messages from files in 'paths' instead.
 */
static void producer_run (FILE *fp, char **paths, int pathcnt) {
        char   *sbuf  = NULL;
        size_t  size = 0;
        ssize_t len;
        char    errstr[512];

        /* Assign per-message delivery report callback. */
        rd_kafka_conf_set_dr_msg_cb(conf.rk_conf, dr_msg_cb);

        /* Create producer */
        if (!(conf.rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf.rk_conf,
                                     errstr, sizeof(errstr))))
                FATAL("Failed to create producer: %s", errstr);

        if (conf.debug)
                rd_kafka_set_log_level(conf.rk, LOG_DEBUG);
        else if (conf.verbosity == 0)
                rd_kafka_set_log_level(conf.rk, 0);

        /* Create topic */
        if (!(conf.rkt = rd_kafka_topic_new(conf.rk, conf.topic,
                                            conf.rkt_conf)))
                FATAL("Failed to create topic %s: %s", conf.topic,
                      rd_kafka_err2str(rd_kafka_errno2err(errno)));

        conf.rk_conf  = NULL;
        conf.rkt_conf = NULL;


        if (pathcnt > 0 && !(conf.flags & CONF_F_LINE)) {
                int i;
                int good = 0;
                /* Read messages from files, each file is its own message. */

                for (i = 0 ; i < pathcnt ; i++)
                        if (produce_file(paths[i]) != -1)
                                good++;

                if (!good)
                        conf.exitcode = 1;
                else if (good < pathcnt)
                        INFO(1, "Failed to produce from %i/%i files\n",
                             pathcnt - good, pathcnt);

        } else {
                /* Read messages from input, delimited by conf.delim */
                while (conf.run &&
                       (len = getdelim(&sbuf, &size, conf.delim, fp)) != -1) {
                        int msgflags = 0;
                        char *buf = sbuf;
                        char *key = NULL;
                        size_t key_len = 0;
                        size_t orig_len = len;

                        if (len == 0)
                                continue;

                        /* Shave off delimiter */
                        if ((int)buf[len-1] == conf.delim)
                                len--;

                        if (len == 0)
                                continue;

                        /* Extract key, if desired and found. */
                        if (conf.flags & CONF_F_KEY_DELIM) {
                                char *t;
                                if ((t = memchr(buf, conf.key_delim, len))) {
                                        key_len = (size_t)(t-sbuf);
                                        key     = buf;
                                        buf    += key_len+1;
                                        len    -= key_len+1;

                                        /* Since buf has been forwarded
                                         * from its initial allocation point
                                         * we must make sure we dont tell
                                         * librdkafka to free it (since the
                                         * address would be wrong). */
                                        msgflags |= RD_KAFKA_MSG_F_COPY;

                                        if (conf.flags & CONF_F_NULL) {
                                                if (len == 0)
                                                        buf = NULL;
                                                if (key_len == 0)
                                                        key = NULL;
                                        }
                                }
                        }

                        if (!(msgflags & RD_KAFKA_MSG_F_COPY) &&
                            len > 1024 && !(conf.flags & CONF_F_TEE)) {
                                /* If message is larger than this arbitrary
                                 * threshold it will be more effective to
                                 * not copy the data but let rdkafka own it
                                 * instead.
                                 *
                                 * Note that CONF_T_TEE must be checked,
                                 * otherwise a possible race might occur.
                                 * */
                                msgflags |= RD_KAFKA_MSG_F_FREE;
                        } else {
                                /* For smaller messages a copy is
                                 * more efficient. */
                                msgflags |= RD_KAFKA_MSG_F_COPY;
                        }

                        /* Produce message */
                        produce(buf, len, key, key_len, msgflags);

                        if (conf.flags & CONF_F_TEE &&
                            fwrite(sbuf, orig_len, 1, stdout) != 1)
                                FATAL("Tee write error for message of %zd bytes: %s",
                                      orig_len, strerror(errno));

                        if (msgflags & RD_KAFKA_MSG_F_FREE) {
                                /* rdkafka owns the allocated buffer
                                 * memory now. */
                                sbuf  = NULL;
                                size = 0;
                        }

                        /* Enforce -c <cnt> */
                        if (stats.tx == conf.msg_cnt)
                                conf.run = 0;
                }

                if (conf.run) {
                        if (!feof(fp))
                                FATAL("Unable to read message: %s",
                                      strerror(errno));
                }
        }

        /* Wait for all messages to be transmitted */
        conf.run = 1;
        while (conf.run && rd_kafka_outq_len(conf.rk))
                rd_kafka_poll(conf.rk, 50);

        rd_kafka_topic_destroy(conf.rkt);
        rd_kafka_destroy(conf.rk);

        if (sbuf)
                free(sbuf);

        if (stats.tx_err_q || stats.tx_err_dr)
                conf.exitcode = 1;
}



/**
 * Consume callback, called for each message consumed.
 */
static void consume_cb (rd_kafka_message_t *rkmessage, void *opaque) {
        FILE *fp = opaque;

        if (!conf.run)
                return;

        if (rkmessage->err) {
                if (rkmessage->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
                        /* Store EOF offset.
                         * If partition is empty and at offset 0,
                         * store future first message (0). */
                        rd_kafka_offset_store(rkmessage->rkt,
                                              rkmessage->partition,
                                              rkmessage->offset == 0 ?
                                              0 : rkmessage->offset-1);
                        if (conf.exit_eof) {
                                if (!part_eof[rkmessage->partition]) {
					/* Stop consuming this partition */
					rd_kafka_consume_stop(rkmessage->rkt,
							      rkmessage->partition);
                                        part_eof[rkmessage->partition] = 1;
                                        part_eof_cnt++;
                                        if (part_eof_cnt >= part_eof_thres)
                                                conf.run = 0;
                                }

                                INFO(1, "Reached end of topic %s [%"PRId32"] "
                                     "at offset %"PRId64"%s\n",
                                     rd_kafka_topic_name(rkmessage->rkt),
                                     rkmessage->partition,
                                     rkmessage->offset,
                                     !conf.run ? ": exiting" : "");
                        }
                        return;
                }

                FATAL("Topic %s [%"PRId32"] error: %s",
                      rd_kafka_topic_name(rkmessage->rkt),
                      rkmessage->partition,
                      rd_kafka_message_errstr(rkmessage));
        }

        /* Print message */
        fmt_msg_output(fp, rkmessage);

        rd_kafka_offset_store(rkmessage->rkt,
                              rkmessage->partition,
                              rkmessage->offset);

        if (++stats.rx == conf.msg_cnt)
                conf.run = 0;
}


/**
 * Run consumer, consuming messages from Kafka and writing to 'fp'.
 */
static void consumer_run (FILE *fp) {
        char    errstr[512];
        rd_kafka_resp_err_t err;
        const rd_kafka_metadata_t *metadata;
        int i;
        rd_kafka_queue_t *rkqu;

        /* Create consumer */
        if (!(conf.rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf.rk_conf,
                                     errstr, sizeof(errstr))))
                FATAL("Failed to create producer: %s", errstr);

        if (conf.debug)
                rd_kafka_set_log_level(conf.rk, LOG_DEBUG);
        else if (conf.verbosity == 0)
                rd_kafka_set_log_level(conf.rk, 0);

        /* The callback-based consumer API's offset store granularity is
         * not good enough for us, disable automatic offset store
         * and do it explicitly per-message in the consume callback instead. */
        if (rd_kafka_topic_conf_set(conf.rkt_conf,
                                    "auto.commit.enable", "false",
                                    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
                FATAL("%s", errstr);

        /* Create topic */
        if (!(conf.rkt = rd_kafka_topic_new(conf.rk, conf.topic,
                                            conf.rkt_conf)))
                FATAL("Failed to create topic %s: %s", conf.topic,
                      rd_kafka_err2str(rd_kafka_errno2err(errno)));

        conf.rk_conf  = NULL;
        conf.rkt_conf = NULL;


        /* Query broker for topic + partition information. */
        if ((err = rd_kafka_metadata(conf.rk, 0, conf.rkt, &metadata, 5000)))
                FATAL("Failed to query metadata for topic %s: %s",
                      rd_kafka_topic_name(conf.rkt), rd_kafka_err2str(err));

        /* Error handling */
        if (metadata->topic_cnt == 0)
                FATAL("No such topic in cluster: %s",
                      rd_kafka_topic_name(conf.rkt));

        if ((err = metadata->topics[0].err))
                FATAL("Topic %s error: %s",
                      rd_kafka_topic_name(conf.rkt), rd_kafka_err2str(err));

        if (metadata->topics[0].partition_cnt == 0)
                FATAL("Topic %s has no partitions",
                      rd_kafka_topic_name(conf.rkt));

        /* If Exit-at-EOF is enabled, set up array to track EOF
         * state for each partition. */
        if (conf.exit_eof) {
                part_eof = calloc(sizeof(*part_eof),
                                  metadata->topics[0].partition_cnt);

                if (conf.partition != RD_KAFKA_PARTITION_UA)
                        part_eof_thres = 1;
                else
                        part_eof_thres = metadata->topics[0].partition_cnt;
        }

        /* Create a shared queue that combines messages from
         * all wanted partitions. */
        rkqu = rd_kafka_queue_new(conf.rk);

        /* Start consuming from all wanted partitions. */
        for (i = 0 ; i < metadata->topics[0].partition_cnt ; i++) {
                int32_t partition = metadata->topics[0].partitions[i].id;

                /* If -p <part> was specified: skip unwanted partitions */
                if (conf.partition != RD_KAFKA_PARTITION_UA &&
                    conf.partition != partition)
                        continue;

                /* Start consumer for this partition */
                if (rd_kafka_consume_start_queue(conf.rkt, partition,
                                                 conf.offset, rkqu) == -1)
                        FATAL("Failed to start consuming "
                              "topic %s [%"PRId32"]: %s",
                              conf.topic, partition,
                              rd_kafka_err2str(rd_kafka_errno2err(errno)));

                if (conf.partition != RD_KAFKA_PARTITION_UA)
                        break;
        }

        if (conf.partition != RD_KAFKA_PARTITION_UA &&
            i == metadata->topics[0].partition_cnt)
                FATAL("Topic %s (with partitions 0..%i): "
                      "partition %i does not exist",
                      rd_kafka_topic_name(conf.rkt),
                      metadata->topics[0].partition_cnt-1,
                      conf.partition);


        /* Read messages from Kafka, write to 'fp'. */
        while (conf.run) {
                rd_kafka_consume_callback_queue(rkqu, 100,
                                                consume_cb, fp);

                /* Poll for errors, etc */
                rd_kafka_poll(conf.rk, 0);
        }

        /* Stop consuming */
        for (i = 0 ; i < metadata->topics[0].partition_cnt ; i++) {
                int32_t partition = metadata->topics[0].partitions[i].id;

                /* If -p <part> was specified: skip unwanted partitions */
                if (conf.partition != RD_KAFKA_PARTITION_UA &&
                    conf.partition != partition)
                        continue;

		/* Dont stop already stopped partitions */
		if (!part_eof || !part_eof[partition])
			rd_kafka_consume_stop(conf.rkt, partition);

                rd_kafka_consume_stop(conf.rkt, partition);
        }

        /* Destroy shared queue */
        rd_kafka_queue_destroy(rkqu);

        /* Wait for outstanding requests to finish. */
        conf.run = 1;
        while (conf.run && rd_kafka_outq_len(conf.rk) > 0)
                rd_kafka_poll(conf.rk, 50);

        rd_kafka_topic_destroy(conf.rkt);
        rd_kafka_destroy(conf.rk);
}


/**
 * Print metadata information
 */
static void metadata_print (const rd_kafka_metadata_t *metadata) {
        int i, j, k;

        printf("Metadata for %s (from broker %"PRId32": %s):\n",
               conf.topic ? : "all topics",
               metadata->orig_broker_id, metadata->orig_broker_name);

        /* Iterate brokers */
        printf(" %i brokers:\n", metadata->broker_cnt);
        for (i = 0 ; i < metadata->broker_cnt ; i++)
                printf("  broker %"PRId32" at %s:%i\n",
                       metadata->brokers[i].id,
                       metadata->brokers[i].host,
                       metadata->brokers[i].port);

        /* Iterate topics */
        printf(" %i topics:\n", metadata->topic_cnt);
        for (i = 0 ; i < metadata->topic_cnt ; i++) {
                const rd_kafka_metadata_topic_t *t = &metadata->topics[i];
                printf("  topic \"%s\" with %i partitions:",
                       t->topic,
                       t->partition_cnt);
                if (t->err) {
                        printf(" %s", rd_kafka_err2str(t->err));
                        if (t->err == RD_KAFKA_RESP_ERR_LEADER_NOT_AVAILABLE)
                                printf(" (try again)");
                }
                printf("\n");

                /* Iterate topic's partitions */
                for (j = 0 ; j < t->partition_cnt ; j++) {
                        const rd_kafka_metadata_partition_t *p;
                        p = &t->partitions[j];
                        printf("    partition %"PRId32", "
                               "leader %"PRId32", replicas: ",
                               p->id, p->leader);

                        /* Iterate partition's replicas */
                        for (k = 0 ; k < p->replica_cnt ; k++)
                                printf("%s%"PRId32,
                                       k > 0 ? ",":"", p->replicas[k]);

                        /* Iterate partition's ISRs */
                        printf(", isrs: ");
                        for (k = 0 ; k < p->isr_cnt ; k++)
                                printf("%s%"PRId32,
                                       k > 0 ? ",":"", p->isrs[k]);
                        if (p->err)
                                printf(", %s\n", rd_kafka_err2str(p->err));
                        else
                                printf("\n");
                }
        }
}


/**
 * Lists metadata
 */
static void metadata_list (void) {
        char    errstr[512];
        rd_kafka_resp_err_t err;
        const rd_kafka_metadata_t *metadata;

        /* Create handle */
        if (!(conf.rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf.rk_conf,
                                     errstr, sizeof(errstr))))
                FATAL("Failed to create producer: %s", errstr);

        rd_kafka_set_logger(conf.rk, rd_kafka_log_print);
        if (conf.debug)
                rd_kafka_set_log_level(conf.rk, LOG_DEBUG);
        else if (conf.verbosity == 0)
                rd_kafka_set_log_level(conf.rk, 0);

        /* Create topic, if specified */
        if (conf.topic &&
            !(conf.rkt = rd_kafka_topic_new(conf.rk, conf.topic,
                                            conf.rkt_conf)))
                FATAL("Failed to create topic %s: %s", conf.topic,
                      rd_kafka_err2str(rd_kafka_errno2err(errno)));

        conf.rk_conf  = NULL;
        conf.rkt_conf = NULL;


        /* Fetch metadata */
        err = rd_kafka_metadata(conf.rk, conf.rkt ? 0 : 1, conf.rkt,
                                &metadata, 5000);
        if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
                FATAL("Failed to acquire metadata: %s", rd_kafka_err2str(err));

        /* Print metadata */
#if ENABLE_JSON
        if (conf.flags & CONF_F_FMT_JSON)
                metadata_print_json(metadata);
        else
#endif
                metadata_print(metadata);

        rd_kafka_metadata_destroy(metadata);

        if (conf.rkt)
                rd_kafka_topic_destroy(conf.rkt);
        rd_kafka_destroy(conf.rk);
}


/**
 * Print usage and exit.
 */
static void __attribute__((noreturn)) usage (const char *argv0, int exitcode,
                                             const char *reason) {

        if (reason)
                printf("Error: %s\n\n", reason);

        printf("Usage: %s <options> [file1 file2 ..]\n"
               "kafkacat - Apache Kafka producer and consumer tool\n"
               "https://github.com/edenhill/kafkacat\n"
               "Copyright (c) 2014-2015, Magnus Edenhill\n"
               "Version %s%s (librdkafka %s)\n"
               "\n"
               "\n"
               "General options:\n"
               "  -C | -P | -L       Mode: Consume, Produce or metadata List\n"
               "  -t <topic>         Topic to consume from, produce to, "
               "or list\n"
               "  -p <partition>     Partition\n"
               "  -b <brokers,..>    Bootstrap broker(s) (host[:port])\n"
               "  -D <delim>         Message delimiter character:\n"
               "                     a-z.. | \\r | \\n | \\t | \\xNN\n"
               "                     Default: \\n\n"
               "  -K <delim>         Key delimiter (same format as -D)\n"
               "  -c <cnt>           Limit message count\n"
               "  -X list            List available librdkafka configuration "
               "properties\n"
               "  -X prop=val        Set librdkafka configuration property.\n"
               "                     Properties prefixed with \"topic.\" are\n"
               "                     applied as topic properties.\n"
               "  -X dump            Dump configuration and exit.\n"
               "  -d <dbg1,...>      Enable librdkafka debugging:\n"
               "                     " RD_KAFKA_DEBUG_CONTEXTS "\n"
               "  -q                 Be quiet (verbosity set to 0)\n"
               "  -v                 Increase verbosity\n"
               "\n"
               "Producer options:\n"
               "  -z snappy|gzip     Message compression. Default: none\n"
               "  -p -1              Use random partitioner\n"
               "  -D <delim>         Delimiter to split input into messages\n"
               "  -K <delim>         Delimiter to split input key and message\n"
               "  -l                 Send messages from a file separated by\n"
               "                     delimiter, as with stdin.\n"
               "                     (only one file allowed)\n"
               "  -T                 Output sent messages to stdout, acting like tee.\n"
               "  -c <cnt>           Exit after producing this number "
               "of messages\n"
               "  -Z                 Send empty messages as NULL messages\n"
               "  file1 file2..      Read messages from files.\n"
               "                     With -l, only one file permitted.\n"
               "                     Otherwise, the entire file contents will\n"
               "                     be sent as one single message.\n"
               "\n"
               "Consumer options:\n"
               "  -o <offset>        Offset to start consuming from:\n"
               "                     beginning | end | stored |\n"
               "                     <value>  (absolute offset) |\n"
               "                     -<value> (relative offset from end)\n"
               "  -e                 Exit successfully when last message "
               "received\n"
               "  -f <fmt..>         Output formatting string, see below.\n"
               "                     Takes precedence over -D and -K.\n"
#if ENABLE_JSON
               "  -J                 Output with JSON envelope\n"
#endif
               "  -D <delim>         Delimiter to separate messages on output\n"
               "  -K <delim>         Print message keys prefixing the message\n"
               "                     with specified delimiter.\n"
               "  -O                 Print message offset using -K delimiter\n"
               "  -c <cnt>           Exit after consuming this number "
               "of messages\n"
               "  -Z                 Print NULL messages and keys as \"%s\""
               "(instead of empty)\n"
               "  -u                 Unbuffered output\n"
               "\n"
               "Metadata options:\n"
               "  -t <topic>         Topic to query (optional)\n"
               "\n"
               "\n"
               "Format string tokens:\n"
               "  %%s                 Message payload\n"
               "  %%S                 Message payload length (or -1 for NULL)\n"
               "  %%k                 Message key\n"
               "  %%K                 Message key length (or -1 for NULL)\n"
               "  %%t                 Topic\n"
               "  %%p                 Partition\n"
               "  %%o                 Message offset\n"
               "  \\n \\r \\t           Newlines, tab\n"
               "  \\xXX \\xNNN         Any ASCII character\n"
               " Example:\n"
               "  -f 'Topic %%t [%%p] at offset %%o: key %%k: %%s\\n'\n"
               "\n"
               "\n"
               "Consumer mode (writes messages to stdout):\n"
               "  kafkacat -b <broker> -t <topic> -p <partition>\n"
               " or:\n"
               "  kafkacat -C -b ...\n"
               "\n"
               "Producer mode (reads messages from stdin):\n"
               "  ... | kafkacat -b <broker> -t <topic> -p <partition>\n"
               " or:\n"
               "  kafkacat -P -b ...\n"
               "\n"
               "Metadata listing:\n"
               "  kafkacat -L -b <broker> [-t <topic>]\n"
               "\n",
               argv0, KAFKACAT_VERSION,
#if ENABLE_JSON
               " (JSON)",
#else
               "",
#endif
               rd_kafka_version_str(),
               conf.null_str
                );
        exit(exitcode);
}


/**
 * Terminate by putting out the run flag.
 */
static void term (int sig) {
        conf.run = 0;
}


/**
 * librdkafka error callback
 */
static void error_cb (rd_kafka_t *rk, int err,
                      const char *reason, void *opaque) {

        if (err == RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN)
                FATAL("%s: %s: terminating", rd_kafka_err2str(err),
                      reason ? reason : "");

        INFO(1, "ERROR: %s: %s\n", rd_kafka_err2str(err),
             reason ? reason : "");
}


/**
 * Parse delimiter string from command line arguments.
 */
static int parse_delim (const char *str) {
        int delim;
        if (!strncmp(str, "\\x", strlen("\\x")))
                delim = strtoul(str+strlen("\\x"), NULL, 16) & 0xff;
        else if (!strcmp(str, "\\n"))
                delim = (int)'\n';
        else if (!strcmp(str, "\\t"))
                delim = (int)'\t';
        else
                delim = (int)*str & 0xff;
        return delim;
}

/**
 * Parse command line arguments
 */
static void argparse (int argc, char **argv) {
        char errstr[512];
        int opt;
        const char *fmt = NULL;
        const char *delim = "\n";
        const char *key_delim = NULL;
        char tmp_fmt[64];

        while ((opt = getopt(argc, argv,
                             "PCLt:p:b:z:o:eD:K:Od:qvX:c:Tuf:Zl"
#if ENABLE_JSON
                             "J"
#endif
                        )) != -1) {
                switch (opt) {
                case 'P':
                case 'C':
                case 'L':
                        conf.mode = opt;
                        break;
                case 't':
                        conf.topic = optarg;
                        break;
                case 'p':
                        conf.partition = atoi(optarg);
                        break;
                case 'b':
                        conf.brokers = optarg;
                        break;
                case 'z':
                        if (rd_kafka_conf_set(conf.rk_conf,
                                              "compression.codec", optarg,
                                              errstr, sizeof(errstr)) !=
                            RD_KAFKA_CONF_OK)
                                FATAL("%s", errstr);
                        break;
                case 'o':
                        if (!strcmp(optarg, "end"))
                                conf.offset = RD_KAFKA_OFFSET_END;
                        else if (!strcmp(optarg, "beginning"))
                                conf.offset = RD_KAFKA_OFFSET_BEGINNING;
                        else if (!strcmp(optarg, "stored"))
                                conf.offset = RD_KAFKA_OFFSET_STORED;
                        else {
                                conf.offset = strtoll(optarg, NULL, 10);
                                if (conf.offset < 0)
                                        conf.offset = RD_KAFKA_OFFSET_TAIL(-conf.offset);
                        }
                        break;
                case 'e':
                        conf.exit_eof = 1;
                        break;
                case 'f':
                        fmt = optarg;
                        break;
#if ENABLE_JSON
                case 'J':
                        conf.flags |= CONF_F_FMT_JSON;
                        break;
#endif
                case 'D':
                        delim = optarg;
                        break;
                case 'K':
                        key_delim = optarg;
                        conf.flags |= CONF_F_KEY_DELIM;
                        break;
                case 'l':
                        conf.flags |= CONF_F_LINE;
                        break;
                case 'O':
                        conf.flags |= CONF_F_OFFSET;
                        break;
                case 'c':
                        conf.msg_cnt = strtoll(optarg, NULL, 10);
                        break;
                case 'Z':
                        conf.flags |= CONF_F_NULL;
                        conf.null_str_len = strlen(conf.null_str);
                        break;
                case 'd':
                        conf.debug = optarg;
                        if (rd_kafka_conf_set(conf.rk_conf, "debug", conf.debug,
                                              errstr, sizeof(errstr)) !=
                            RD_KAFKA_CONF_OK)
                                FATAL("%s", errstr);
                        break;
                case 'q':
                        conf.verbosity = 0;
                        break;
                case 'v':
                        conf.verbosity++;
                        break;
                case 'T':
                        conf.flags |= CONF_F_TEE;
                        break;
                case 'u':
                        setbuf(stdout, NULL);
                        break;
                case 'X':
                {
                        char *name, *val;
                        rd_kafka_conf_res_t res;

                        if (!strcmp(optarg, "list") ||
                            !strcmp(optarg, "help")) {
                                rd_kafka_conf_properties_show(stdout);
                                exit(0);
                        }

                        if (!strcmp(optarg, "dump")) {
                                conf.conf_dump = 1;
                                continue;
                        }

                        name = optarg;
                        if (!(val = strchr(name, '='))) {
                                fprintf(stderr, "%% Expected "
                                        "-X property=value, not %s, "
                                        "use -X list to display available "
                                        "properties\n", name);
                                exit(1);
                        }

                        *val = '\0';
                        val++;

                        res = RD_KAFKA_CONF_UNKNOWN;
                        /* Try "topic." prefixed properties on topic
                         * conf first, and then fall through to global if
                         * it didnt match a topic configuration property. */
                        if (!strncmp(name, "topic.", strlen("topic.")))
                                res = rd_kafka_topic_conf_set(conf.rkt_conf,
                                                              name+
                                                              strlen("topic."),
                                                              val,
                                                              errstr,
                                                              sizeof(errstr));

                        if (res == RD_KAFKA_CONF_UNKNOWN)
                                res = rd_kafka_conf_set(conf.rk_conf, name, val,
                                                        errstr, sizeof(errstr));

                        if (res != RD_KAFKA_CONF_OK)
                                FATAL("%s", errstr);
                }
                break;

                default:
                        usage(argv[0], 1, "unknown argument");
                        break;
                }
        }


        if (!conf.brokers)
                usage(argv[0], 1, "-b <broker,..> missing");

        /* Decide mode if not specified */
        if (!conf.mode) {
                if (isatty(STDIN_FILENO))
                        conf.mode = 'C';
                else
                        conf.mode = 'P';
                INFO(1, "Auto-selecting %s mode (use -P or -C to override)\n",
                     conf.mode == 'C' ? "Consumer":"Producer");
        }


        if (conf.mode != 'L' && !conf.topic)
                usage(argv[0], 1, "-t <topic> missing");

        if (rd_kafka_conf_set(conf.rk_conf, "metadata.broker.list",
                              conf.brokers, errstr, sizeof(errstr)) !=
            RD_KAFKA_CONF_OK)
                usage(argv[0], 1, errstr);

        rd_kafka_conf_set_error_cb(conf.rk_conf, error_cb);

        fmt_init();


        if (conf.mode == 'C') {
                if (!fmt) {
                        if ((conf.flags & CONF_F_FMT_JSON)) {
                                /* For JSON the format string is simply the
                                 * output object delimiter (e.g., newline). */
                                fmt = delim;
                        } else {
                                if (key_delim)
                                        snprintf(tmp_fmt, sizeof(tmp_fmt),
                                                 "%%k%s%%s%s",
                                                 key_delim, delim);
                                else
                                        snprintf(tmp_fmt, sizeof(tmp_fmt),
                                                 "%%s%s", delim);
                                fmt = tmp_fmt;
                        }
                }

                fmt_parse(fmt);

        } else if (conf.mode == 'P') {
                conf.delim = parse_delim(delim);
		if (conf.flags & CONF_F_KEY_DELIM)
			conf.key_delim = parse_delim(key_delim);
        }
}


/**
 * Dump current rdkafka configuration to stdout.
 */
static void conf_dump (void) {
        const char **arr;
        size_t cnt;
        int pass;

        for (pass = 0 ; pass < 2 ; pass++) {
                int i;

                if (pass == 0) {
                        arr = rd_kafka_conf_dump(conf.rk_conf, &cnt);
                        printf("# Global config\n");
                } else {
                        printf("# Topic config\n");
                        arr = rd_kafka_topic_conf_dump(conf.rkt_conf, &cnt);
                }

                for (i = 0 ; i < cnt ; i += 2)
                        printf("%s = %s\n",
                               arr[i], arr[i+1]);

                printf("\n");

                rd_kafka_conf_dump_free(arr, cnt);
        }
}


int main (int argc, char **argv) {
        char tmp[16];
        FILE *in = stdin;

        signal(SIGINT, term);
        signal(SIGTERM, term);
        signal(SIGPIPE, term);

        /* Create config containers */
        conf.rk_conf  = rd_kafka_conf_new();
        conf.rkt_conf = rd_kafka_topic_conf_new();

        /*
         * Default config
         */
        /* Enable quick termination of librdkafka */
        snprintf(tmp, sizeof(tmp), "%i", SIGIO);
        rd_kafka_conf_set(conf.rk_conf, "internal.termination.signal",
                          tmp, NULL, 0);

        /* Parse command line arguments */
        argparse(argc, argv);

        /* Dump configuration and exit, if so desired. */
        if (conf.conf_dump) {
                conf_dump();
                exit(0);
        }

        if (optind < argc) {
                if (conf.mode != 'P')
                        usage(argv[0], 1,
                              "file list only allowed in produce mode");
                else if ((conf.flags & CONF_F_LINE) && argc - optind > 1)
                        FATAL("Only one file allowed for line mode (-l)");
                else if (conf.flags & CONF_F_LINE) {
                        in = fopen(argv[optind], "r");
                        if (in == NULL)
                                FATAL("Cannot open %s: %s", argv[optind],
                                      strerror(errno));
                }
        }

        /* Run according to mode */
        switch (conf.mode)
        {
        case 'C':
                consumer_run(stdout);
                break;

        case 'P':
                producer_run(in, &argv[optind], argc-optind);
                break;

        case 'L':
                metadata_list();
                break;

        default:
                usage(argv[0], 0, NULL);
                break;
        }

        if (in != stdin)
                fclose(in);

        rd_kafka_wait_destroyed(5000);

        fmt_term();

        exit(conf.exitcode);
}
