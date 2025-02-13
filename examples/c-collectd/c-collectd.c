#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "nDPIsrvd.h"

#define DEFAULT_COLLECTD_EXEC_INST "nDPIsrvd"
//#define GENERATE_TIMESTAMP 1

#define LOG(flags, format, ...)                                                                                        \
    if (quiet == 0)                                                                                                    \
    {                                                                                                                  \
        fprintf(stderr, format, __VA_ARGS__);                                                                          \
        fprintf(stderr, "%s", "\n");                                                                                   \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
        syslog(flags, format, __VA_ARGS__);                                                                            \
    }

struct flow_user_data
{
    nDPIsrvd_ull last_flow_src_l4_payload_len;
    nDPIsrvd_ull last_flow_dst_l4_payload_len;
};

static int main_thread_shutdown = 0;
static int collectd_timerfd = -1;
static pid_t collectd_pid;

static char * serv_optarg = NULL;
static char * collectd_hostname = NULL;
static char * collectd_interval = NULL;
static char * instance_name = NULL;
static nDPIsrvd_ull collectd_interval_ull = 0uL;
static int quiet = 0;

static struct
{
    uint64_t flow_new_count;
    uint64_t flow_end_count;
    uint64_t flow_idle_count;
    uint64_t flow_guessed_count;
    uint64_t flow_detected_count;
    uint64_t flow_detection_update_count;
    uint64_t flow_not_detected_count;

    uint64_t flow_src_total_bytes;
    uint64_t flow_dst_total_bytes;
    uint64_t flow_risky_count;

    uint64_t flow_breed_safe_count;
    uint64_t flow_breed_acceptable_count;
    uint64_t flow_breed_fun_count;
    uint64_t flow_breed_unsafe_count;
    uint64_t flow_breed_potentially_dangerous_count;
    uint64_t flow_breed_dangerous_count;
    uint64_t flow_breed_unrated_count;
    uint64_t flow_breed_unknown_count;

    uint64_t flow_category_media_count;
    uint64_t flow_category_vpn_count;
    uint64_t flow_category_email_count;
    uint64_t flow_category_data_transfer_count;
    uint64_t flow_category_web_count;
    uint64_t flow_category_social_network_count;
    uint64_t flow_category_download_count;
    uint64_t flow_category_game_count;
    uint64_t flow_category_chat_count;
    uint64_t flow_category_voip_count;
    uint64_t flow_category_database_count;
    uint64_t flow_category_remote_access_count;
    uint64_t flow_category_cloud_count;
    uint64_t flow_category_network_count;
    uint64_t flow_category_collaborative_count;
    uint64_t flow_category_rpc_count;
    uint64_t flow_category_streaming_count;
    uint64_t flow_category_system_count;
    uint64_t flow_category_software_update_count;
    uint64_t flow_category_music_count;
    uint64_t flow_category_video_count;
    uint64_t flow_category_shopping_count;
    uint64_t flow_category_productivity_count;
    uint64_t flow_category_file_sharing_count;
    uint64_t flow_category_mining_count;
    uint64_t flow_category_malware_count;
    uint64_t flow_category_advertisment_count;
    uint64_t flow_category_other_count;
    uint64_t flow_category_unknown_count;

    uint64_t flow_l3_ip4_count;
    uint64_t flow_l3_ip6_count;
    uint64_t flow_l3_other_count;
    uint64_t flow_l4_tcp_count;
    uint64_t flow_l4_udp_count;
    uint64_t flow_l4_icmp_count;
    uint64_t flow_l4_other_count;
} collectd_statistics = {};

#ifdef ENABLE_MEMORY_PROFILING
void nDPIsrvd_memprof_log(char const * const format, ...)
{
    va_list ap;

    va_start(ap, format);
    fprintf(stderr, "%s", "nDPIsrvd MemoryProfiler: ");
    vfprintf(stderr, format, ap);
    fprintf(stderr, "%s\n", "");
    va_end(ap);
}
#endif

static int set_collectd_timer(void)
{
    const time_t interval = collectd_interval_ull * 1000;
    struct itimerspec its;
    its.it_value.tv_sec = interval / 1000;
    its.it_value.tv_nsec = (interval % 1000) * 1000000;
    its.it_interval.tv_nsec = 0;
    its.it_interval.tv_sec = 0;

    errno = 0;
    return timerfd_settime(collectd_timerfd, 0, &its, NULL);
}

static int create_collectd_timer(void)
{
    collectd_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (collectd_timerfd < 0)
    {
        return 1;
    }

    return set_collectd_timer();
}

static void sighandler(int signum)
{
    LOG(LOG_DAEMON | LOG_NOTICE, "Received SIGNAL %d", signum);

    if (main_thread_shutdown == 0)
    {
        LOG(LOG_DAEMON | LOG_NOTICE, "%s", "Shutting down ..");
        main_thread_shutdown = 1;
    }
}

static int parse_options(int argc, char ** argv, struct nDPIsrvd_socket * const sock)
{
    int opt;

    static char const usage[] =
        "Usage: %s "
        "[-s host] [-c hostname] [-n collectd-instance-name] [-i interval] [-q]\n\n"
        "\t-s\tDestination where nDPIsrvd is listening on.\n"
        "\t-c\tCollectd hostname.\n"
        "\t  \tThis value defaults to the environment variable COLLECTD_HOSTNAME.\n"
        "\t-n\tName of the collectd(-exec) instance.\n"
        "\t  \tDefaults to: " DEFAULT_COLLECTD_EXEC_INST
        "\n"
        "\t-i\tInterval between print statistics to stdout.\n"
        "\t  \tThis value defaults to the environment variable COLLECTD_INTERVAL.\n"
        "\t-q\tDo not print anything except collectd statistics.\n"
        "\t  \tAutomatically enabled if environment variables mentioned above are set.\n";

    while ((opt = getopt(argc, argv, "hs:c:n:i:q")) != -1)
    {
        switch (opt)
        {
            case 's':
                free(serv_optarg);
                serv_optarg = strdup(optarg);
                break;
            case 'c':
                free(collectd_hostname);
                collectd_hostname = strdup(optarg);
                break;
            case 'n':
                free(instance_name);
                instance_name = strdup(optarg);
                break;
            case 'i':
                free(collectd_interval);
                collectd_interval = strdup(optarg);
                break;
            case 'q':
                quiet = 1;
                break;
            default:
                LOG(LOG_DAEMON | LOG_ERR, usage, argv[0]);
                return 1;
        }
    }

    if (serv_optarg == NULL)
    {
        serv_optarg = strdup(DISTRIBUTOR_UNIX_SOCKET);
    }

    if (collectd_hostname == NULL)
    {
        collectd_hostname = getenv("COLLECTD_HOSTNAME");
        if (collectd_hostname == NULL)
        {
            collectd_hostname = strdup("localhost");
        }
    }

    if (instance_name == NULL)
    {
        instance_name = strdup(DEFAULT_COLLECTD_EXEC_INST);
    }

    if (collectd_interval == NULL)
    {
        collectd_interval = getenv("COLLECTD_INTERVAL");
        if (collectd_interval == NULL)
        {
            collectd_interval = strdup("60");
        }
    }

    if (str_value_to_ull(collectd_interval, &collectd_interval_ull) != CONVERSION_OK)
    {
        LOG(LOG_DAEMON | LOG_ERR, "Collectd interval `%s' is not a valid number", collectd_interval);
        return 1;
    }

    if (nDPIsrvd_setup_address(&sock->address, serv_optarg) != 0)
    {
        LOG(LOG_DAEMON | LOG_ERR, "Could not parse address `%s'", serv_optarg);
        return 1;
    }

    if (optind < argc)
    {
        LOG(LOG_DAEMON | LOG_ERR, "%s", "Unexpected argument after options");
        if (quiet == 0)
        {
            LOG(0, "%s", "");
            LOG(0, usage, argv[0]);
        }
        return 1;
    }

    return 0;
}

#ifdef GENERATE_TIMESTAMP
#define COLLECTD_PUTVAL_N_FORMAT(name) "PUTVAL \"%s/exec-%s/gauge-" #name "\" interval=%llu %llu:%llu\n"
#define COLLECTD_PUTVAL_N(value)                                                                                       \
    collectd_hostname, instance_name, collectd_interval_ull, (unsigned long long int)now,                              \
        (unsigned long long int)collectd_statistics.value
#else
#define COLLECTD_PUTVAL_N_FORMAT(name) "PUTVAL \"%s/exec-%s/gauge-" #name "\" interval=%llu N:%llu\n"
#define COLLECTD_PUTVAL_N(value)                                                                                       \
    collectd_hostname, instance_name, collectd_interval_ull, (unsigned long long int)collectd_statistics.value
#endif
static void print_collectd_exec_output(void)
{
#ifdef GENERATE_TIMESTAMP
    time_t now = time(NULL);
#endif

    printf(COLLECTD_PUTVAL_N_FORMAT(flow_new_count) COLLECTD_PUTVAL_N_FORMAT(flow_end_count)
               COLLECTD_PUTVAL_N_FORMAT(flow_idle_count) COLLECTD_PUTVAL_N_FORMAT(flow_guessed_count)
                   COLLECTD_PUTVAL_N_FORMAT(flow_detected_count) COLLECTD_PUTVAL_N_FORMAT(flow_detection_update_count)
                       COLLECTD_PUTVAL_N_FORMAT(flow_not_detected_count) COLLECTD_PUTVAL_N_FORMAT(flow_src_total_bytes)
                           COLLECTD_PUTVAL_N_FORMAT(flow_dst_total_bytes) COLLECTD_PUTVAL_N_FORMAT(flow_risky_count),

           COLLECTD_PUTVAL_N(flow_new_count),
           COLLECTD_PUTVAL_N(flow_end_count),
           COLLECTD_PUTVAL_N(flow_idle_count),
           COLLECTD_PUTVAL_N(flow_guessed_count),
           COLLECTD_PUTVAL_N(flow_detected_count),
           COLLECTD_PUTVAL_N(flow_detection_update_count),
           COLLECTD_PUTVAL_N(flow_not_detected_count),
           COLLECTD_PUTVAL_N(flow_src_total_bytes),
           COLLECTD_PUTVAL_N(flow_dst_total_bytes),
           COLLECTD_PUTVAL_N(flow_risky_count));

    printf(COLLECTD_PUTVAL_N_FORMAT(flow_breed_safe_count) COLLECTD_PUTVAL_N_FORMAT(flow_breed_acceptable_count)
               COLLECTD_PUTVAL_N_FORMAT(flow_breed_fun_count) COLLECTD_PUTVAL_N_FORMAT(flow_breed_unsafe_count)
                   COLLECTD_PUTVAL_N_FORMAT(flow_breed_potentially_dangerous_count)
                       COLLECTD_PUTVAL_N_FORMAT(flow_breed_dangerous_count)
                           COLLECTD_PUTVAL_N_FORMAT(flow_breed_unrated_count)
                               COLLECTD_PUTVAL_N_FORMAT(flow_breed_unknown_count),

           COLLECTD_PUTVAL_N(flow_breed_safe_count),
           COLLECTD_PUTVAL_N(flow_breed_acceptable_count),
           COLLECTD_PUTVAL_N(flow_breed_fun_count),
           COLLECTD_PUTVAL_N(flow_breed_unsafe_count),
           COLLECTD_PUTVAL_N(flow_breed_potentially_dangerous_count),
           COLLECTD_PUTVAL_N(flow_breed_dangerous_count),
           COLLECTD_PUTVAL_N(flow_breed_unrated_count),
           COLLECTD_PUTVAL_N(flow_breed_unknown_count));

    printf(
        COLLECTD_PUTVAL_N_FORMAT(flow_category_media_count) COLLECTD_PUTVAL_N_FORMAT(
            flow_category_vpn_count) COLLECTD_PUTVAL_N_FORMAT(flow_category_email_count)
            COLLECTD_PUTVAL_N_FORMAT(flow_category_data_transfer_count) COLLECTD_PUTVAL_N_FORMAT(
                flow_category_web_count) COLLECTD_PUTVAL_N_FORMAT(flow_category_social_network_count)
                COLLECTD_PUTVAL_N_FORMAT(flow_category_download_count) COLLECTD_PUTVAL_N_FORMAT(
                    flow_category_game_count) COLLECTD_PUTVAL_N_FORMAT(flow_category_chat_count)
                    COLLECTD_PUTVAL_N_FORMAT(flow_category_voip_count) COLLECTD_PUTVAL_N_FORMAT(
                        flow_category_database_count) COLLECTD_PUTVAL_N_FORMAT(flow_category_remote_access_count)
                        COLLECTD_PUTVAL_N_FORMAT(flow_category_cloud_count) COLLECTD_PUTVAL_N_FORMAT(
                            flow_category_network_count) COLLECTD_PUTVAL_N_FORMAT(flow_category_collaborative_count)
                            COLLECTD_PUTVAL_N_FORMAT(flow_category_rpc_count) COLLECTD_PUTVAL_N_FORMAT(
                                flow_category_streaming_count) COLLECTD_PUTVAL_N_FORMAT(flow_category_system_count)
                                COLLECTD_PUTVAL_N_FORMAT(flow_category_software_update_count) COLLECTD_PUTVAL_N_FORMAT(
                                    flow_category_music_count) COLLECTD_PUTVAL_N_FORMAT(flow_category_video_count)
                                    COLLECTD_PUTVAL_N_FORMAT(flow_category_shopping_count)
                                        COLLECTD_PUTVAL_N_FORMAT(flow_category_productivity_count)
                                            COLLECTD_PUTVAL_N_FORMAT(flow_category_file_sharing_count)
                                                COLLECTD_PUTVAL_N_FORMAT(flow_category_mining_count)
                                                    COLLECTD_PUTVAL_N_FORMAT(flow_category_malware_count)
                                                        COLLECTD_PUTVAL_N_FORMAT(flow_category_advertisment_count)
                                                            COLLECTD_PUTVAL_N_FORMAT(flow_category_other_count)
                                                                COLLECTD_PUTVAL_N_FORMAT(flow_category_unknown_count),

        COLLECTD_PUTVAL_N(flow_category_media_count),
        COLLECTD_PUTVAL_N(flow_category_vpn_count),
        COLLECTD_PUTVAL_N(flow_category_email_count),
        COLLECTD_PUTVAL_N(flow_category_data_transfer_count),
        COLLECTD_PUTVAL_N(flow_category_web_count),
        COLLECTD_PUTVAL_N(flow_category_social_network_count),
        COLLECTD_PUTVAL_N(flow_category_download_count),
        COLLECTD_PUTVAL_N(flow_category_game_count),
        COLLECTD_PUTVAL_N(flow_category_chat_count),
        COLLECTD_PUTVAL_N(flow_category_voip_count),
        COLLECTD_PUTVAL_N(flow_category_database_count),
        COLLECTD_PUTVAL_N(flow_category_remote_access_count),
        COLLECTD_PUTVAL_N(flow_category_cloud_count),
        COLLECTD_PUTVAL_N(flow_category_network_count),
        COLLECTD_PUTVAL_N(flow_category_collaborative_count),
        COLLECTD_PUTVAL_N(flow_category_rpc_count),
        COLLECTD_PUTVAL_N(flow_category_streaming_count),
        COLLECTD_PUTVAL_N(flow_category_system_count),
        COLLECTD_PUTVAL_N(flow_category_software_update_count),
        COLLECTD_PUTVAL_N(flow_category_music_count),
        COLLECTD_PUTVAL_N(flow_category_video_count),
        COLLECTD_PUTVAL_N(flow_category_shopping_count),
        COLLECTD_PUTVAL_N(flow_category_productivity_count),
        COLLECTD_PUTVAL_N(flow_category_file_sharing_count),
        COLLECTD_PUTVAL_N(flow_category_mining_count),
        COLLECTD_PUTVAL_N(flow_category_malware_count),
        COLLECTD_PUTVAL_N(flow_category_advertisment_count),
        COLLECTD_PUTVAL_N(flow_category_other_count),
        COLLECTD_PUTVAL_N(flow_category_unknown_count));

    printf(COLLECTD_PUTVAL_N_FORMAT(flow_l3_ip4_count) COLLECTD_PUTVAL_N_FORMAT(flow_l3_ip6_count)
               COLLECTD_PUTVAL_N_FORMAT(flow_l3_other_count) COLLECTD_PUTVAL_N_FORMAT(flow_l4_tcp_count)
                   COLLECTD_PUTVAL_N_FORMAT(flow_l4_udp_count) COLLECTD_PUTVAL_N_FORMAT(flow_l4_icmp_count)
                       COLLECTD_PUTVAL_N_FORMAT(flow_l4_other_count),

           COLLECTD_PUTVAL_N(flow_l3_ip4_count),
           COLLECTD_PUTVAL_N(flow_l3_ip6_count),
           COLLECTD_PUTVAL_N(flow_l3_other_count),
           COLLECTD_PUTVAL_N(flow_l4_tcp_count),
           COLLECTD_PUTVAL_N(flow_l4_udp_count),
           COLLECTD_PUTVAL_N(flow_l4_icmp_count),
           COLLECTD_PUTVAL_N(flow_l4_other_count));

    memset(&collectd_statistics, 0, sizeof(collectd_statistics));
}

static int mainloop(int epollfd, struct nDPIsrvd_socket * const sock)
{
    struct epoll_event events[32];
    size_t const events_size = sizeof(events) / sizeof(events[0]);

    while (main_thread_shutdown == 0)
    {
        int nready = epoll_wait(epollfd, events, events_size, -1);

        for (int i = 0; i < nready; i++)
        {
            if (events[i].events & EPOLLERR)
            {
                LOG(LOG_DAEMON | LOG_ERR, "Epoll event error: %s", (errno != 0 ? strerror(errno) : "EPOLLERR"));
                break;
            }

            if (events[i].data.fd == collectd_timerfd)
            {
                uint64_t expirations;

                /*
                 * Check if collectd parent process is still running.
                 * May happen if collectd was killed with singals e.g. SIGKILL.
                 */
                if (getppid() != collectd_pid)
                {
                    LOG(LOG_DAEMON | LOG_ERR, "Parent process %d exited. Nothing left to do here, bye.", collectd_pid);
                    return 1;
                }

                errno = 0;
                if (read(collectd_timerfd, &expirations, sizeof(expirations)) != sizeof(expirations))
                {
                    LOG(LOG_DAEMON | LOG_ERR, "Could not read timer expirations: %s", strerror(errno));
                    return 1;
                }
                if (set_collectd_timer() != 0)
                {
                    LOG(LOG_DAEMON | LOG_ERR, "Could not set timer: %s", strerror(errno));
                    return 1;
                }

                print_collectd_exec_output();
            }
            else if (events[i].data.fd == sock->fd)
            {
                errno = 0;
                enum nDPIsrvd_read_return read_ret = nDPIsrvd_read(sock);
                if (read_ret != READ_OK)
                {
                    LOG(LOG_DAEMON | LOG_ERR, "nDPIsrvd read failed with: %s", nDPIsrvd_enum_to_string(read_ret));
                    return 1;
                }

                enum nDPIsrvd_parse_return parse_ret = nDPIsrvd_parse_all(sock);
                if (parse_ret != PARSE_NEED_MORE_DATA)
                {
                    LOG(LOG_DAEMON | LOG_ERR, "nDPIsrvd parse failed with: %s", nDPIsrvd_enum_to_string(parse_ret));
                    return 1;
                }
            }
        }
    }

    return 0;
}

static enum nDPIsrvd_callback_return captured_json_callback(struct nDPIsrvd_socket * const sock,
                                                            struct nDPIsrvd_instance * const instance,
                                                            struct nDPIsrvd_thread_data * const thread_data,
                                                            struct nDPIsrvd_flow * const flow)
{
    (void)sock;
    (void)instance;
    (void)thread_data;
    (void)flow;

    struct nDPIsrvd_json_token const * const flow_event_name = TOKEN_GET_SZ(sock, "flow_event_name");
    struct flow_user_data * const flow_user_data = (struct flow_user_data *)flow->flow_user_data;

    if (flow_user_data != NULL)
    {
        nDPIsrvd_ull total_bytes_ull[2] = {0, 0};

        if (TOKEN_VALUE_TO_ULL(TOKEN_GET_SZ(sock, "flow_src_tot_l4_payload_len"), &total_bytes_ull[0]) ==
                CONVERSION_OK &&
            TOKEN_VALUE_TO_ULL(TOKEN_GET_SZ(sock, "flow_dst_tot_l4_payload_len"), &total_bytes_ull[1]) == CONVERSION_OK)
        {
            collectd_statistics.flow_src_total_bytes +=
                total_bytes_ull[0] - flow_user_data->last_flow_src_l4_payload_len;
            collectd_statistics.flow_dst_total_bytes +=
                total_bytes_ull[1] - flow_user_data->last_flow_dst_l4_payload_len;

            flow_user_data->last_flow_src_l4_payload_len = total_bytes_ull[0];
            flow_user_data->last_flow_dst_l4_payload_len = total_bytes_ull[1];
        }
    }

    if (TOKEN_VALUE_EQUALS_SZ(flow_event_name, "new") != 0)
    {
        collectd_statistics.flow_new_count++;

        struct nDPIsrvd_json_token const * const l3_proto = TOKEN_GET_SZ(sock, "l3_proto");
        if (TOKEN_VALUE_EQUALS_SZ(l3_proto, "ip4") != 0)
        {
            collectd_statistics.flow_l3_ip4_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(l3_proto, "ip6") != 0)
        {
            collectd_statistics.flow_l3_ip6_count++;
        }
        else if (l3_proto != NULL)
        {
            collectd_statistics.flow_l3_other_count++;
        }

        struct nDPIsrvd_json_token const * const l4_proto = TOKEN_GET_SZ(sock, "l4_proto");
        if (TOKEN_VALUE_EQUALS_SZ(l4_proto, "tcp") != 0)
        {
            collectd_statistics.flow_l4_tcp_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(l4_proto, "udp") != 0)
        {
            collectd_statistics.flow_l4_udp_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(l4_proto, "icmp") != 0)
        {
            collectd_statistics.flow_l4_icmp_count++;
        }
        else if (l4_proto != NULL)
        {
            collectd_statistics.flow_l4_other_count++;
        }
    }
    else if (TOKEN_VALUE_EQUALS_SZ(flow_event_name, "end") != 0)
    {
        collectd_statistics.flow_end_count++;
    }
    else if (TOKEN_VALUE_EQUALS_SZ(flow_event_name, "idle") != 0)
    {
        collectd_statistics.flow_idle_count++;
    }
    else if (TOKEN_VALUE_EQUALS_SZ(flow_event_name, "guessed") != 0)
    {
        collectd_statistics.flow_guessed_count++;
    }
    else if (TOKEN_VALUE_EQUALS_SZ(flow_event_name, "detected") != 0)
    {
        collectd_statistics.flow_detected_count++;

        if (TOKEN_GET_SZ(sock, "flow_risk") != NULL)
        {
            collectd_statistics.flow_risky_count++;
        }

        struct nDPIsrvd_json_token const * const breed = TOKEN_GET_SZ(sock, "breed");
        if (TOKEN_VALUE_EQUALS_SZ(breed, "Safe") != 0)
        {
            collectd_statistics.flow_breed_safe_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(breed, "Acceptable") != 0)
        {
            collectd_statistics.flow_breed_acceptable_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(breed, "Fun") != 0)
        {
            collectd_statistics.flow_breed_fun_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(breed, "Unsafe") != 0)
        {
            collectd_statistics.flow_breed_unsafe_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(breed, "Potentially Dangerous") != 0)
        {
            collectd_statistics.flow_breed_potentially_dangerous_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(breed, "Dangerous") != 0)
        {
            collectd_statistics.flow_breed_dangerous_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(breed, "Unrated") != 0)
        {
            collectd_statistics.flow_breed_unrated_count++;
        }
        else
        {
            collectd_statistics.flow_breed_unknown_count++;
        }

        struct nDPIsrvd_json_token const * const category = TOKEN_GET_SZ(sock, "category");
        if (TOKEN_VALUE_EQUALS_SZ(category, "Media") != 0)
        {
            collectd_statistics.flow_category_media_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "VPN") != 0)
        {
            collectd_statistics.flow_category_vpn_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Email") != 0)
        {
            collectd_statistics.flow_category_email_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "DataTransfer") != 0)
        {
            collectd_statistics.flow_category_data_transfer_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Web") != 0)
        {
            collectd_statistics.flow_category_web_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "SocialNetwork") != 0)
        {
            collectd_statistics.flow_category_social_network_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Download-FileTransfer-FileSharing") != 0)
        {
            collectd_statistics.flow_category_download_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Game") != 0)
        {
            collectd_statistics.flow_category_game_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Chat") != 0)
        {
            collectd_statistics.flow_category_chat_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "VoIP") != 0)
        {
            collectd_statistics.flow_category_voip_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Database") != 0)
        {
            collectd_statistics.flow_category_database_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "RemoteAccess") != 0)
        {
            collectd_statistics.flow_category_remote_access_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Cloud") != 0)
        {
            collectd_statistics.flow_category_cloud_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Network") != 0)
        {
            collectd_statistics.flow_category_network_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Collaborative") != 0)
        {
            collectd_statistics.flow_category_collaborative_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "RPC") != 0)
        {
            collectd_statistics.flow_category_rpc_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Streaming") != 0)
        {
            collectd_statistics.flow_category_streaming_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "System") != 0)
        {
            collectd_statistics.flow_category_system_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "SoftwareUpdate") != 0)
        {
            collectd_statistics.flow_category_software_update_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Music") != 0)
        {
            collectd_statistics.flow_category_music_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Video") != 0)
        {
            collectd_statistics.flow_category_video_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Shopping") != 0)
        {
            collectd_statistics.flow_category_shopping_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Productivity") != 0)
        {
            collectd_statistics.flow_category_productivity_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "FileSharing") != 0)
        {
            collectd_statistics.flow_category_file_sharing_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Mining") != 0)
        {
            collectd_statistics.flow_category_mining_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Malware") != 0)
        {
            collectd_statistics.flow_category_malware_count++;
        }
        else if (TOKEN_VALUE_EQUALS_SZ(category, "Advertisement") != 0)
        {
            collectd_statistics.flow_category_advertisment_count++;
        }
        else if (category != NULL)
        {
            collectd_statistics.flow_category_other_count++;
        }
        else
        {
            collectd_statistics.flow_category_unknown_count++;
        }
    }
    else if (TOKEN_VALUE_EQUALS_SZ(flow_event_name, "detection-update") != 0)
    {
        collectd_statistics.flow_detection_update_count++;
    }
    else if (TOKEN_VALUE_EQUALS_SZ(flow_event_name, "not-detected") != 0)
    {
        collectd_statistics.flow_not_detected_count++;
    }

    return CALLBACK_OK;
}

int main(int argc, char ** argv)
{
    int retval = 1, epollfd = -1;

    openlog("nDPIsrvd-collectd", LOG_CONS, LOG_DAEMON);

    struct nDPIsrvd_socket * sock =
        nDPIsrvd_socket_init(0, 0, 0, sizeof(struct flow_user_data), captured_json_callback, NULL, NULL);
    if (sock == NULL)
    {
        LOG(LOG_DAEMON | LOG_ERR, "%s", "nDPIsrvd socket memory allocation failed!");
        return 1;
    }

    if (parse_options(argc, argv, sock) != 0)
    {
        goto failure;
    }

    if (getenv("COLLECTD_HOSTNAME") == NULL && getenv("COLLECTD_INTERVAL") == NULL)
    {
        LOG(LOG_DAEMON | LOG_NOTICE, "Recv buffer size: %u", NETWORK_BUFFER_MAX_SIZE);
        LOG(LOG_DAEMON | LOG_NOTICE, "Connecting to `%s'..", serv_optarg);
    }
    else
    {
        quiet = 1;
        LOG(LOG_DAEMON | LOG_NOTICE, "Collectd hostname: %s", getenv("COLLECTD_HOSTNAME"));
        LOG(LOG_DAEMON | LOG_NOTICE, "Collectd interval: %llu", collectd_interval_ull);
    }

    if (setvbuf(stdout, NULL, _IONBF, 0) != 0)
    {
        LOG(LOG_DAEMON | LOG_ERR,
            "Could not set stdout unbuffered: %s. Collectd may receive too old PUTVALs and complain.",
            strerror(errno));
    }

    enum nDPIsrvd_connect_return connect_ret = nDPIsrvd_connect(sock);
    if (connect_ret != CONNECT_OK)
    {
        LOG(LOG_DAEMON | LOG_ERR, "nDPIsrvd socket connect to %s failed!", serv_optarg);
        goto failure;
    }

    if (nDPIsrvd_set_nonblock(sock) != 0)
    {
        LOG(LOG_DAEMON | LOG_ERR, "nDPIsrvd set nonblock failed: %s", strerror(errno));
        goto failure;
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGPIPE, SIG_IGN);

    collectd_pid = getppid();

    epollfd = epoll_create1(0);
    if (epollfd < 0)
    {
        LOG(LOG_DAEMON | LOG_ERR, "Error creating epoll: %s", strerror(errno));
        goto failure;
    }

    if (create_collectd_timer() != 0)
    {
        LOG(LOG_DAEMON | LOG_ERR, "Error creating timer: %s", strerror(errno));
        goto failure;
    }

    {
        struct epoll_event timer_event = {.data.fd = collectd_timerfd, .events = EPOLLIN};
        if (epoll_ctl(epollfd, EPOLL_CTL_ADD, collectd_timerfd, &timer_event) < 0)
        {
            LOG(LOG_DAEMON | LOG_ERR, "Error adding JSON fd to epoll: %s", strerror(errno));
            goto failure;
        }
    }

    {
        struct epoll_event socket_event = {.data.fd = sock->fd, .events = EPOLLIN};
        if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sock->fd, &socket_event) < 0)
        {
            LOG(LOG_DAEMON | LOG_ERR, "Error adding nDPIsrvd socket fd to epoll: %s", strerror(errno));
            goto failure;
        }
    }

    LOG(LOG_DAEMON | LOG_NOTICE, "%s", "Initialization succeeded.");
    retval = mainloop(epollfd, sock);

failure:
    nDPIsrvd_socket_free(&sock);
    close(collectd_timerfd);
    close(epollfd);
    closelog();

    return retval;
}
