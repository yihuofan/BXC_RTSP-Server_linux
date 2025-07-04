/*
移除Windows特定头文件：删除 <WinSock2.h>, <WS2tcpip.h>, <windows.h>
添加Linux网络头文件：添加 <sys/socket.h>, <netinet/in.h>, <arpa/inet.h>, <unistd.h>
修改函数调用：Sleep(40) 改为 usleep(40000)、closesocket() 改为 close()
移除Windows Socket初始化：删除 WSAStartup() 和 WSACleanup() 调用
修复条件判断：
修复了 if (serverRtpSockfd) 为 if (serverRtpSockfd > 0)

添加了转换MP4到H264的函数 convertMp4ToH264
*/

//
// Created by sun on 10/11/21.
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include "rtp.h"

#define MP4_FILE_NAME "../data/test.mp4"
#define H264_FILE_NAME "../data/test.h264"
#define SERVER_PORT 8554
#define SERVER_RTP_PORT 55532
#define SERVER_RTCP_PORT 55533
#define BUF_MAX_SIZE (1024 * 1024)

// 将MP4文件转换为H264文件
static int convertMp4ToH264(const char *mp4File, const char *h264File)
{
    char command[512];
    int ret;
    // 构造FFmpeg命令
    snprintf(command, sizeof(command),
             "ffmpeg -i %s -c:v libx264 -an -f h264 %s",
             mp4File, h264File);

    // 执行命令
    ret = system(command);
    if (ret != 0)
    {
        printf("FFmpeg转换失败，返回值: %d\n", ret);
        return -1;
    }
    return 0;
}

static int createTcpSocket()
{
    int sockfd;
    int on = 1;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        return -1;

    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));

    return sockfd;
}

static int createUdpSocket()
{
    int sockfd;
    int on = 1;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        return -1;

    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));

    return sockfd;
}

static int bindSocketAddr(int sockfd, const char *ip, int port)
{
    struct sockaddr_in addr;

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(struct sockaddr)) < 0)
        return -1;

    return 0;
}

static int acceptClient(int sockfd, char *ip, int *port)
{
    int clientfd;
    socklen_t len = 0;
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    len = sizeof(addr);

    clientfd = accept(sockfd, (struct sockaddr *)&addr, &len);
    if (clientfd < 0)
        return -1;

    strcpy(ip, inet_ntoa(addr.sin_addr));
    *port = ntohs(addr.sin_port);

    return clientfd;
}

static inline int startCode3(char *buf)
{
    if (buf[0] == 0 && buf[1] == 0 && buf[2] == 1)
        return 1;
    else
        return 0;
}

static inline int startCode4(char *buf)
{
    if (buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 1)
        return 1;
    else
        return 0;
}

static char *findNextStartCode(char *buf, int len)
{
    int i;

    if (len < 3)
        return NULL;

    for (i = 0; i < len - 3; ++i)
    {
        if (startCode3(buf) || startCode4(buf))
            return buf;

        ++buf;
    }

    if (startCode3(buf))
        return buf;

    return NULL;
}

static int getFrameFromH264File(FILE *fp, char *frame, int size)
{
    int rSize, frameSize;
    char *nextStartCode;

    if (fp < 0)
        return -1;

    rSize = fread(frame, 1, size, fp);

    if (!startCode3(frame) && !startCode4(frame))
        return -1;

    nextStartCode = findNextStartCode(frame + 3, rSize - 3);
    if (!nextStartCode)
    {
        // lseek(fd, 0, SEEK_SET);
        // frameSize = rSize;
        return -1;
    }
    else
    {
        frameSize = (nextStartCode - frame);
        fseek(fp, frameSize - rSize, SEEK_CUR);
    }

    return frameSize;
}

static int rtpSendH264Frame(int serverRtpSockfd, const char *ip, int16_t port,
                            struct RtpPacket *rtpPacket, char *frame, uint32_t frameSize)
{

    uint8_t naluType; // nalu第一个字节
    int sendBytes = 0;
    int ret;

    naluType = frame[0];

    printf("frameSize=%d \n", frameSize);

    if (frameSize <= RTP_MAX_PKT_SIZE) // nalu长度小于最大包长：单一NALU单元模式
    {

        //*   0 1 2 3 4 5 6 7 8 9
        //*  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        //*  |F|NRI|  Type   | a single NAL unit ... |
        //*  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

        memcpy(rtpPacket->payload, frame, frameSize);
        ret = rtpSendPacketOverUdp(serverRtpSockfd, ip, port, rtpPacket, frameSize);
        if (ret < 0)
            return -1;

        rtpPacket->rtpHeader.seq++;
        sendBytes += ret;
        if ((naluType & 0x1F) == 7 || (naluType & 0x1F) == 8) // 如果是SPS、PPS就不需要加时间戳
            goto out;
    }
    else // nalu长度小于最大包场：分片模式
    {

        //*  0                   1                   2
        //*  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3
        //* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        //* | FU indicator  |   FU header   |   FU payload   ...  |
        //* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

        //*     FU Indicator
        //*    0 1 2 3 4 5 6 7
        //*   +-+-+-+-+-+-+-+-+
        //*   |F|NRI|  Type   |
        //*   +---------------+

        //*      FU Header
        //*    0 1 2 3 4 5 6 7
        //*   +-+-+-+-+-+-+-+-+
        //*   |S|E|R|  Type   |
        //*   +---------------+

        int pktNum = frameSize / RTP_MAX_PKT_SIZE;        // 有几个完整的包
        int remainPktSize = frameSize % RTP_MAX_PKT_SIZE; // 剩余不完整包的大小
        int i, pos = 1;

        // 发送完整的包
        for (i = 0; i < pktNum; i++)
        {
            rtpPacket->payload[0] = (naluType & 0x60) | 28;
            rtpPacket->payload[1] = naluType & 0x1F;

            if (i == 0)                                     // 第一包数据
                rtpPacket->payload[1] |= 0x80;              // start
            else if (remainPktSize == 0 && i == pktNum - 1) // 最后一包数据
                rtpPacket->payload[1] |= 0x40;              // end

            memcpy(rtpPacket->payload + 2, frame + pos, RTP_MAX_PKT_SIZE);
            ret = rtpSendPacketOverUdp(serverRtpSockfd, ip, port, rtpPacket, RTP_MAX_PKT_SIZE + 2);
            if (ret < 0)
                return -1;

            rtpPacket->rtpHeader.seq++;
            sendBytes += ret;
            pos += RTP_MAX_PKT_SIZE;
        }

        // 发送剩余的数据
        if (remainPktSize > 0)
        {
            rtpPacket->payload[0] = (naluType & 0x60) | 28;
            rtpPacket->payload[1] = naluType & 0x1F;
            rtpPacket->payload[1] |= 0x40; // end

            memcpy(rtpPacket->payload + 2, frame + pos, remainPktSize + 2);
            ret = rtpSendPacketOverUdp(serverRtpSockfd, ip, port, rtpPacket, remainPktSize + 2);
            if (ret < 0)
                return -1;

            rtpPacket->rtpHeader.seq++;
            sendBytes += ret;
        }
    }
    rtpPacket->rtpHeader.timestamp += 90000 / 25;
out:

    return sendBytes;
}

static int handleCmd_OPTIONS(char *result, int cseq)
{
    sprintf(result, "RTSP/1.0 200 OK\r\n"
                    "CSeq: %d\r\n"
                    "Public: OPTIONS, DESCRIBE, SETUP, PLAY\r\n"
                    "\r\n",
            cseq);

    return 0;
}

static int handleCmd_DESCRIBE(char *result, int cseq, char *url)
{
    char sdp[500];
    char localIp[100];

    sscanf(url, "rtsp://%[^:]:", localIp);

    sprintf(sdp, "v=0\r\n"
                 "o=- 9%ld 1 IN IP4 %s\r\n"
                 "t=0 0\r\n"
                 "a=control:*\r\n"
                 "m=video 0 RTP/AVP 96\r\n"
                 "a=rtpmap:96 H264/90000\r\n"
                 "a=control:track0\r\n",
            time(NULL), localIp);

    sprintf(result, "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
                    "Content-Base: %s\r\n"
                    "Content-type: application/sdp\r\n"
                    "Content-length: %zu\r\n\r\n"
                    "%s",
            cseq,
            url,
            strlen(sdp),
            sdp);

    return 0;
}

static int handleCmd_SETUP(char *result, int cseq, int clientRtpPort)
{
    sprintf(result, "RTSP/1.0 200 OK\r\n"
                    "CSeq: %d\r\n"
                    "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
                    "Session: 66334873\r\n"
                    "\r\n",
            cseq,
            clientRtpPort,
            clientRtpPort + 1,
            SERVER_RTP_PORT,
            SERVER_RTCP_PORT);

    return 0;
}

static int handleCmd_PLAY(char *result, int cseq)
{
    sprintf(result, "RTSP/1.0 200 OK\r\n"
                    "CSeq: %d\r\n"
                    "Range: npt=0.000-\r\n"
                    "Session: 66334873; timeout=10\r\n\r\n",
            cseq);

    return 0;
}

static void doClient(int clientSockfd, const char *clientIP, int clientPort)
{

    char method[40];
    char url[100];
    char version[40];
    int CSeq;

    int serverRtpSockfd = -1, serverRtcpSockfd = -1;
    int clientRtpPort, clientRtcpPort;
    char *rBuf = (char *)malloc(BUF_MAX_SIZE);
    char *sBuf = (char *)malloc(BUF_MAX_SIZE);

    while (true)
    {
        int recvLen;

        recvLen = recv(clientSockfd, rBuf, BUF_MAX_SIZE, 0);
        if (recvLen <= 0)
        {
            break;
        }

        rBuf[recvLen] = '\0';
        printf("%s rBuf = %s \n", __FUNCTION__, rBuf);

        const char *sep = "\n";
        char *line = strtok(rBuf, sep);
        while (line)
        {
            if (strstr(line, "OPTIONS") ||
                strstr(line, "DESCRIBE") ||
                strstr(line, "SETUP") ||
                strstr(line, "PLAY"))
            {

                if (sscanf(line, "%s %s %s\r\n", method, url, version) != 3)
                {
                    // error
                }
            }
            else if (strstr(line, "CSeq"))
            {
                if (sscanf(line, "CSeq: %d\r\n", &CSeq) != 1)
                {
                    // error
                }
            }
            else if (!strncmp(line, "Transport:", strlen("Transport:")))
            {
                // Transport: RTP/AVP/UDP;unicast;client_port=13358-13359
                // Transport: RTP/AVP;unicast;client_port=13358-13359

                if (sscanf(line, "Transport: RTP/AVP/UDP;unicast;client_port=%d-%d\r\n",
                           &clientRtpPort, &clientRtcpPort) != 2)
                {
                    // error
                    printf("parse Transport error \n");
                }
            }
            line = strtok(NULL, sep);
        }

        if (!strcmp(method, "OPTIONS"))
        {
            if (handleCmd_OPTIONS(sBuf, CSeq))
            {
                printf("failed to handle options\n");
                break;
            }
        }
        else if (!strcmp(method, "DESCRIBE"))
        {
            if (handleCmd_DESCRIBE(sBuf, CSeq, url))
            {
                printf("failed to handle describe\n");
                break;
            }
        }
        else if (!strcmp(method, "SETUP"))
        {
            if (handleCmd_SETUP(sBuf, CSeq, clientRtpPort))
            {
                printf("failed to handle setup\n");
                break;
            }

            serverRtpSockfd = createUdpSocket();
            serverRtcpSockfd = createUdpSocket();

            if (serverRtpSockfd < 0 || serverRtcpSockfd < 0)
            {
                printf("failed to create udp socket\n");
                break;
            }

            if (bindSocketAddr(serverRtpSockfd, "0.0.0.0", SERVER_RTP_PORT) < 0 ||
                bindSocketAddr(serverRtcpSockfd, "0.0.0.0", SERVER_RTCP_PORT) < 0)
            {
                printf("failed to bind addr\n");
                break;
            }
        }
        else if (!strcmp(method, "PLAY"))
        {
            if (handleCmd_PLAY(sBuf, CSeq))
            {
                printf("failed to handle play\n");
                break;
            }
        }
        else
        {
            printf("未定义的method = %s \n", method);
            break;
        }
        printf("sBuf = %s \n", sBuf);
        printf("%s sBuf = %s \n", __FUNCTION__, sBuf);

        send(clientSockfd, sBuf, strlen(sBuf), 0);

        // 开始播放，发送RTP包
        if (!strcmp(method, "PLAY"))
        {

            int frameSize, startCode;
            char *frame = (char *)malloc(500000);
            struct RtpPacket *rtpPacket = (struct RtpPacket *)malloc(500000);
            FILE *fp = fopen(H264_FILE_NAME, "rb");
            if (!fp)
            {
                printf("读取 %s 失败\n", H264_FILE_NAME);
                break;
            }
            rtpHeaderInit(rtpPacket, 0, 0, 0, RTP_VESION, RTP_PAYLOAD_TYPE_H264, 0,
                          0, 0, 0x88923423);

            printf("start play\n");
            printf("client ip:%s\n", clientIP);
            printf("client port:%d\n", clientRtpPort);

            while (true)
            {
                frameSize = getFrameFromH264File(fp, frame, 500000);
                if (frameSize < 0)
                {
                    printf("读取%s结束,frameSize=%d \n", H264_FILE_NAME, frameSize);
                    break;
                }

                if (startCode3(frame))
                    startCode = 3;
                else
                    startCode = 4;

                frameSize -= startCode;
                rtpSendH264Frame(serverRtpSockfd, clientIP, clientRtpPort,
                                 rtpPacket, frame + startCode, frameSize);

                usleep(40000); // 1000/25 * 1000
            }
            free(frame);
            free(rtpPacket);

            break;
        }

        memset(method, 0, sizeof(method) / sizeof(char));
        memset(url, 0, sizeof(url) / sizeof(char));
        CSeq = 0;
    }

    close(clientSockfd);
    if (serverRtpSockfd > 0)
    {
        close(serverRtpSockfd);
    }
    if (serverRtcpSockfd > 0)
    {
        close(serverRtcpSockfd);
    }

    free(rBuf);
    free(sBuf);
}

int main(int argc, char *argv[])
{
    int rtspServerSockfd;
    convertMp4ToH264(MP4_FILE_NAME, H264_FILE_NAME);// 转换MP4文件为H264文件
    rtspServerSockfd = createTcpSocket();
    if (rtspServerSockfd < 0)
    {
        printf("failed to create tcp socket\n");
        return -1;
    }

    if (bindSocketAddr(rtspServerSockfd, "0.0.0.0", SERVER_PORT) < 0)
    {
        printf("failed to bind addr\n");
        return -1;
    }

    if (listen(rtspServerSockfd, 10) < 0)
    {
        printf("failed to listen\n");
        return -1;
    }

    printf("%s rtsp://127.0.0.1:%d\n", __FILE__, SERVER_PORT);

    while (true)
    {
        int clientSockfd;
        char clientIp[40];
        int clientPort;

        clientSockfd = acceptClient(rtspServerSockfd, clientIp, &clientPort);
        if (clientSockfd < 0)
        {
            printf("failed to accept client\n");
            return -1;
        }

        printf("accept client;client ip:%s,client port:%d\n", clientIp, clientPort);

        doClient(clientSockfd, clientIp, clientPort);
    }
    close(rtspServerSockfd);

    return 0;
}