#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <netinet/if_ether.h>

#define if_name "eth1"
#define TEST_ETHERTYPE 0xeeee

int main(int argc, char *argv[])
{
    if (argc != 2) {
        printf("Usage: %s <frame_length>\n", argv[0]);
        return 1;
    }

    uint16_t frame_length = atoi(argv[1]);
    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sockfd == -1) {
        perror("socket error");
        return 1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, if_name, IFNAMSIZ - 1);

    if (ioctl(sockfd, SIOCGIFINDEX, &ifr) == -1) {
        perror("ioctl error");
        close(sockfd);
        return 1;
    }
    int ifindex = ifr.ifr_ifindex;

    struct sockaddr_ll addr;
    memset(&addr, 0, sizeof(addr));
    addr.sll_family   = AF_PACKET;
    addr.sll_ifindex  = ifindex;
    addr.sll_halen    = ETH_ALEN;

    addr.sll_addr[0] = 0xa1;
    addr.sll_addr[1] = 0xa2;
    addr.sll_addr[2] = 0xa3;
    addr.sll_addr[3] = 0xa4;
    addr.sll_addr[4] = 0xa5;
    addr.sll_addr[5] = 0xa6;

    unsigned char frame[1600];
    memset(frame, 0, sizeof(frame));

    struct ether_header *eh = (struct ether_header *)frame;

    eh->ether_dhost[0] = 0xa1;
    eh->ether_dhost[1] = 0xa2;
    eh->ether_dhost[2] = 0xa3;
    eh->ether_dhost[3] = 0xa4;
    eh->ether_dhost[4] = 0xa5;
    eh->ether_dhost[5] = 0xa6;

    eh->ether_shost[0] = 0xf1;
    eh->ether_shost[1] = 0xf2;
    eh->ether_shost[2] = 0xf3;
    eh->ether_shost[3] = 0xf4;
    eh->ether_shost[4] = 0xf5;
    eh->ether_shost[5] = 0xf6;

    eh->ether_type = htons(TEST_ETHERTYPE);

    for (int i = 0; i < frame_length - ETH_HLEN; i++) {
        switch (i % 6) {
            case 0:
                frame[ETH_HLEN + i] = 0xaa;
                break;
            case 1:
                frame[ETH_HLEN + i] = 0xbb;
                break;
            case 2:
                frame[ETH_HLEN + i] = 0xcc;
                break;
            case 3:
                frame[ETH_HLEN + i] = 0xdd;
                break;
            case 4:
                frame[ETH_HLEN + i] = 0xee;
                break;
            case 5:
                frame[ETH_HLEN + i] = 0xff;
                break;
            default:
                perror("fatal error!!");
                close(sockfd);
                return 1;
        }
    }

    frame[frame_length - 2] = 0xca;
    frame[frame_length - 1] = 0xfe;

    ssize_t sent = sendto(sockfd,
                          frame,
                          frame_length,
                          0,
                          (struct sockaddr *)&addr,
                          sizeof(addr));
    if (sent == -1) {
        perror("sendto error");
        close(sockfd);
        return 1;
    }

    printf("sent %zd bytes\n", sent);

    close(sockfd);
    return 0;
}
