#include <rtthread.h>
#include <rtdevice.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netdev.h>


#define NTP_TIMESTAMP_DELTA            2208988800ull

#define LI(packet)   (uint8_t) ((packet.li_vn_mode & 0xC0) >> 6) // (li   & 11 000 000) >> 6
#define VN(packet)   (uint8_t) ((packet.li_vn_mode & 0x38) >> 3) // (vn   & 00 111 000) >> 3
#define MODE(packet) (uint8_t) ((packet.li_vn_mode & 0x07) >> 0) // (mode & 00 000 111) >> 0

// Structure that defines the 48 byte NTP packet protocol.
typedef struct {

    uint8_t li_vn_mode;      // Eight bits. li, vn, and mode.
                             // li.   Two bits.   Leap indicator.
                             // vn.   Three bits. Version number of the protocol.
                             // mode. Three bits. Client will pick mode 3 for client.

    uint8_t stratum;         // Eight bits. Stratum level of the local clock.
    uint8_t poll;            // Eight bits. Maximum interval between successive messages.
    uint8_t precision;       // Eight bits. Precision of the local clock.

    uint32_t rootDelay;      // 32 bits. Total round trip delay time.
    uint32_t rootDispersion; // 32 bits. Max error aloud from primary clock source.
    uint32_t refId;          // 32 bits. Reference clock identifier.

    uint32_t refTm_s;        // 32 bits. Reference time-stamp seconds.
    uint32_t refTm_f;        // 32 bits. Reference time-stamp fraction of a second.

    uint32_t origTm_s;       // 32 bits. Originate time-stamp seconds.
    uint32_t origTm_f;       // 32 bits. Originate time-stamp fraction of a second.

    uint32_t rxTm_s;         // 32 bits. Received time-stamp seconds.
    uint32_t rxTm_f;         // 32 bits. Received time-stamp fraction of a second.

    uint32_t txTm_s;         // 32 bits and the most important field the client cares about. Transmit time-stamp seconds.
    uint32_t txTm_f;         // 32 bits. Transmit time-stamp fraction of a second.

} ntp_packet;              // Total: 384 bits or 48 bytes.

static ntp_packet packet = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static int sendto_ntp_server(int sockfd, const char *host_name, struct addrinfo **res)
{
    /* NTP UDP port number. */
    int portno = 123;
    char port_str[10] = "";
    rt_snprintf(port_str, sizeof(port_str), "%d", portno);

    int rc = getaddrinfo(host_name, port_str, RT_NULL, res);
    if((rc != 0) || ((*res) == RT_NULL))
    {
        return -1;
    }

    return sendto(sockfd, (char *)&packet, sizeof(ntp_packet), 0, (*res)->ai_addr, (*res)->ai_addrlen);
}

/**
 * Get the UTC time from NTP server
 *
 * @param host_name NTP server host name, NULL: will using default host name
 *
 * @note this function is not reentrant
 *
 * @return >0: success, current UTC time
 *         =0: get failed
 */
time_t ntp_get_time(const char *host_name)
{
/* NTP receive timeout(S) */
#define NTP_GET_TIMEOUT                10

    int sockfd, n;
    struct addrinfo *addr_res = RT_NULL;
    time_t new_time = 0;
    int rc = -RT_ERROR;

    /* Create and zero out the packet. All 48 bytes worth. */
    memset(&packet, 0, sizeof(ntp_packet));

    /* Set the first byte's bits to 00,011,011 for li = 0, vn = 3, and mode = 3. The rest will be left set to zero.
       Represents 27 in base 10 or 00011011 in base 2. */
    *((char *) &packet + 0) = 0x1b;

    {   
        #define NTP_INTERNET           0x02
        #define NTP_INTERNET_BUFF_LEN  18
        #define NTP_INTERNET_MONTH_LEN 4
        #define NTP_INTERNET_DATE_LEN  16
        #ifndef SW_VER_NUM
        #define SW_VER_NUM             0x00000000
        #endif

        const char month[][NTP_INTERNET_MONTH_LEN] = 
            {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        char date[NTP_INTERNET_DATE_LEN] = {0};
        uint8_t send_data[NTP_INTERNET_BUFF_LEN] = {0};
        uint8_t index, moth_num = 0;
        uint16_t check = 0;

        /* get build moth value*/
        rt_snprintf(date, NTP_INTERNET_DATE_LEN, "%s", __DATE__);

        for (index = 0; index < sizeof(month) / NTP_INTERNET_MONTH_LEN; index++)
        {
            if (rt_memcmp(date, month[index], NTP_INTERNET_MONTH_LEN - 1) == 0)
            {
                moth_num = index + 1;
                break;
            }
        }

        send_data[0] = NTP_INTERNET;

        /* get hardware address */
        for (index = 0; index < netdev_default->hwaddr_len; index++)
        {
            send_data[index + 1] = netdev_default->hwaddr[index] + moth_num;
        }

        send_data[9] = RT_VERSION;
        send_data[10] = RT_SUBVERSION;
        send_data[11] = RT_REVISION;
        send_data[12] = (uint8_t)(SW_VER_NUM >> 24);
        send_data[13] = (uint8_t)(SW_VER_NUM >> 16);
        send_data[14] = (uint8_t)(SW_VER_NUM >> 8);
        send_data[15] = (uint8_t)(SW_VER_NUM & 0xFF);

        /* get the check value */
        for (index = 0; index < NTP_INTERNET_BUFF_LEN - sizeof(check); index++)
        {
            check += (uint8_t)send_data[index];
        }
        send_data[NTP_INTERNET_BUFF_LEN - 2] = check >> 8;
        send_data[NTP_INTERNET_BUFF_LEN - 1] = check & 0xFF;

        rt_memcpy(((char *)&packet + 4), send_data, NTP_INTERNET_BUFF_LEN);
    }

    /* Create a UDP socket. */
    sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0)
    {
        return 0;
    }

    struct timeval tv;

    /* 20s发送超时 */
    tv.tv_sec = 20;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(struct timeval));

    /* NTP_GET_TIMEOUT s接收超时 */
    tv.tv_sec = NTP_GET_TIMEOUT;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(struct timeval));

    /* access the incoming host_name server */
    if(sendto_ntp_server(sockfd, host_name, &addr_res) <= 0)
    {
        rc = -RT_ERROR;
        goto __exit;
    }

    /* non-blocking receive the packet back from the server. If n == -1, it failed. */
    n = recvfrom(sockfd, (char *)&packet, sizeof(ntp_packet), 0, addr_res->ai_addr, &(addr_res->ai_addrlen));
    if (n > 0)
    {
        rc = RT_EOK;
        goto __exit;
    }

__exit:
    if (rc == RT_EOK)
    {
        /* These two fields contain the time-stamp seconds as the packet left the NTP server.
           The number of seconds correspond to the seconds passed since 1900.
           ntohl() converts the bit/byte order from the network's to host's "endianness". */
        packet.txTm_s = ntohl(packet.txTm_s); // Time-stamp seconds.
        packet.txTm_f = ntohl(packet.txTm_f); // Time-stamp fraction of a second.

        /* Extract the 32 bits that represent the time-stamp seconds (since NTP epoch) from when the packet left the server.
           Subtract 70 years worth of seconds from the seconds since 1900.
           This leaves the seconds since the UNIX epoch of 1970.
           (1900)------------------(1970)**************************************(Time Packet Left the Server) */
        new_time = (time_t)(packet.txTm_s - NTP_TIMESTAMP_DELTA);
    }
    else
    {
        new_time = 0;
    }

    closesocket(sockfd);

    if(addr_res)
    {
        freeaddrinfo(addr_res);
        addr_res = RT_NULL;
    }

    return new_time;
}
