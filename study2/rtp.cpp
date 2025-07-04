/*
移除Windows特定头文件： <WinSock2.h>、 <WS2tcpip.h>、<windows.h>

添加Linux网络头文件：<sys/types.h> - 系统数据类型、<sys/socket.h> - socket函数、<netinet/in.h> - 网络地址结构、<arpa/inet.h> - IP地址转换函数、<unistd.h> - Unix标准函数、<stdlib.h> - 标准库函数(malloc, free)、<string.h> - 字符串函数(memcpy)
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "rtp.h"

void rtpHeaderInit(struct RtpPacket *rtpPacket, uint8_t csrcLen, uint8_t extension,
                   uint8_t padding, uint8_t version, uint8_t payloadType, uint8_t marker,
                   uint16_t seq, uint32_t timestamp, uint32_t ssrc)
{
    rtpPacket->rtpHeader.csrcLen = csrcLen;
    rtpPacket->rtpHeader.extension = extension;
    rtpPacket->rtpHeader.padding = padding;
    rtpPacket->rtpHeader.version = version;
    rtpPacket->rtpHeader.payloadType = payloadType;
    rtpPacket->rtpHeader.marker = marker;
    rtpPacket->rtpHeader.seq = seq;
    rtpPacket->rtpHeader.timestamp = timestamp;
    rtpPacket->rtpHeader.ssrc = ssrc;
}

int rtpSendPacketOverTcp(int clientSockfd, struct RtpPacket *rtpPacket, uint32_t dataSize)
{
    rtpPacket->rtpHeader.seq = htons(rtpPacket->rtpHeader.seq);
    rtpPacket->rtpHeader.timestamp = htonl(rtpPacket->rtpHeader.timestamp);
    rtpPacket->rtpHeader.ssrc = htonl(rtpPacket->rtpHeader.ssrc);

    uint32_t rtpSize = RTP_HEADER_SIZE + dataSize;
    char *tempBuf = (char *)malloc(4 + rtpSize);
    tempBuf[0] = 0x24; //$
    tempBuf[1] = 0x00;
    tempBuf[2] = (uint8_t)(((rtpSize) & 0xFF00) >> 8);
    tempBuf[3] = (uint8_t)((rtpSize) & 0xFF);
    memcpy(tempBuf + 4, (char *)rtpPacket, rtpSize);

    int ret = send(clientSockfd, tempBuf, 4 + rtpSize, 0);

    rtpPacket->rtpHeader.seq = ntohs(rtpPacket->rtpHeader.seq);
    rtpPacket->rtpHeader.timestamp = ntohl(rtpPacket->rtpHeader.timestamp);
    rtpPacket->rtpHeader.ssrc = ntohl(rtpPacket->rtpHeader.ssrc);

    free(tempBuf);
    tempBuf = NULL;

    return ret;
}

int rtpSendPacketOverUdp(int serverRtpSockfd, const char *ip, int16_t port, struct RtpPacket *rtpPacket, uint32_t dataSize)
{
    struct sockaddr_in addr;
    int ret;

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);

    rtpPacket->rtpHeader.seq = htons(rtpPacket->rtpHeader.seq); // 从主机字节顺序转变成网络字节顺序
    rtpPacket->rtpHeader.timestamp = htonl(rtpPacket->rtpHeader.timestamp);
    rtpPacket->rtpHeader.ssrc = htonl(rtpPacket->rtpHeader.ssrc);

    ret = sendto(serverRtpSockfd, (char *)rtpPacket, dataSize + RTP_HEADER_SIZE, 0,
                 (struct sockaddr *)&addr, sizeof(addr));

    rtpPacket->rtpHeader.seq = ntohs(rtpPacket->rtpHeader.seq);
    rtpPacket->rtpHeader.timestamp = ntohl(rtpPacket->rtpHeader.timestamp);
    rtpPacket->rtpHeader.ssrc = ntohl(rtpPacket->rtpHeader.ssrc);

    return ret;
}
