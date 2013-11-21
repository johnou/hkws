#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <stdbool.h>
#include <pthread.h>

#include <X11/Xproto.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/record.h>

#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "cwebsocket/lib/websocket.h"

#define PORT 8088
#define BUF_LEN 0xFFFF

static int gClientSocket = 0;

typedef union {
    unsigned char type;
    xEvent event;
    xResourceReq req;
    xGenericReply reply;
    xError error;
    xConnSetupPrefix setup;
} XRecordDatum;

void error(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

int safeSend(int clientSocket, const uint8_t * buffer, size_t bufferSize)
{
    ssize_t written = send(clientSocket, buffer, bufferSize, 0);
    if (written == -1) {
        close(clientSocket);
        perror("Send failed");
        return EXIT_FAILURE;
    }
    if (written != bufferSize) {
        close(clientSocket);
        perror("Written not all bytes");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

void eventCallback(XPointer priv, XRecordInterceptData * hook)
{
    if (hook->category != XRecordFromServer) {
        XRecordFreeData(hook);
        return;
    }

    size_t frameSize = BUF_LEN;
    uint8_t buffer[BUF_LEN];
    uint8_t payload[64];

    XRecordDatum *data = (XRecordDatum *) hook->data;

    int keyCode = (int) data->event.u.u.detail;
    int eventType = (int) data->type;

    if (keyCode == 37) { // left control
        switch (eventType) {
            case KeyPress:
            case KeyRelease:
            if (gClientSocket != 0) {
                frameSize = BUF_LEN;
                memset(buffer, 0, BUF_LEN);
                frameSize = sprintf((char *) payload, "{\"keycode\" : %d, \"type\" : %d}", keyCode, eventType);
                wsMakeFrame(payload, frameSize, buffer, &frameSize, WS_TEXT_FRAME);
                safeSend(gClientSocket, buffer, frameSize);
            }
            break;
        }
    }

    XRecordFreeData(hook);
}

void clientWorker(int clientSocket)
{
    uint8_t buffer[BUF_LEN];
    memset(buffer, 0, BUF_LEN);
    size_t readedLength = 0;
    size_t frameSize = BUF_LEN;
    enum wsState state = WS_STATE_OPENING;
    uint8_t *data = NULL;
    size_t dataSize = 0;
    enum wsFrameType frameType = WS_INCOMPLETE_FRAME;
    struct handshake hs;
    nullHandshake(&hs);
    gClientSocket = clientSocket;

    #define prepareBuffer frameSize = BUF_LEN; memset(buffer, 0, BUF_LEN);
    #define initNewFrame frameType = WS_INCOMPLETE_FRAME; readedLength = 0; memset(buffer, 0, BUF_LEN);

    while (frameType == WS_INCOMPLETE_FRAME) {
        ssize_t readed = recv(clientSocket, buffer + readedLength, BUF_LEN - readedLength, 0);
        if (!readed) {
            close(clientSocket);
            perror("Recv failed\n");
            return;
        }
        readedLength += readed;
        assert(readedLength <= BUF_LEN);

        if (state == WS_STATE_OPENING) {
            frameType = wsParseHandshake(buffer, readedLength, &hs);
        } else {
            frameType = wsParseInputFrame(buffer, readedLength, &data, &dataSize);
        }

        if ((frameType == WS_INCOMPLETE_FRAME && readedLength == BUF_LEN) || frameType == WS_ERROR_FRAME) {
            if (frameType == WS_INCOMPLETE_FRAME)
                printf("Buffer too small\n");
            else
                printf("Error in incoming frame\n");

            if (state == WS_STATE_OPENING) {
                prepareBuffer;
                frameSize = sprintf((char *)buffer,
                                    "HTTP/1.1 400 Bad Request\r\n"
                                    "%s%s\r\n\r\n",
                                    versionField,
                                    version);
                safeSend(clientSocket, buffer, frameSize);
                break;
            } else {
                prepareBuffer;
                wsMakeFrame(NULL, 0, buffer, &frameSize, WS_CLOSING_FRAME);
                if (safeSend(clientSocket, buffer, frameSize) == EXIT_FAILURE)
                    break;
                state = WS_STATE_CLOSING;
                initNewFrame;
            }
        }

        if (state == WS_STATE_OPENING) {
            assert(frameType == WS_OPENING_FRAME);
            if (frameType == WS_OPENING_FRAME) {
                if (strcmp(hs.resource, "/events") != 0) {
                    frameSize = sprintf((char *)buffer, "HTTP/1.1 404 Not Found\r\n\r\n");
                    safeSend(clientSocket, buffer, frameSize);
                    break;
                }

                prepareBuffer;
                wsGetHandshakeAnswer(&hs, buffer, &frameSize);
                if (safeSend(clientSocket, buffer, frameSize) == EXIT_FAILURE)
                    break;
                state = WS_STATE_NORMAL;
                initNewFrame;
            }
        } else {
            if (frameType == WS_CLOSING_FRAME) {
                if (state == WS_STATE_CLOSING) {
                    break;
                } else {
                    prepareBuffer;
                    wsMakeFrame(NULL, 0, buffer, &frameSize, WS_CLOSING_FRAME);
                    safeSend(clientSocket, buffer, frameSize);
                    break;
                }
            }
        }
    }

    close(clientSocket);
}

void *socketLoop(void *ptr)
{
    int listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket == -1) {
        error("Create socket failed!\n");
    }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(PORT);

    int on = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char *) &on, sizeof(on));
    if (bind(listenSocket, (struct sockaddr *) &local, sizeof(local)) == -1) {
        error("Bind failed!\n");
    }

    if (listen(listenSocket, 1) == -1) {
        error("Listen failed!\n");
    }
    printf("Opened %s:%d\n", inet_ntoa(local.sin_addr), ntohs(local.sin_port));

    while (true) {
        struct sockaddr_in remote;
        socklen_t sockaddrLen = sizeof(remote);
        int clientSocket = accept(listenSocket, (struct sockaddr *) &remote, &sockaddrLen);
        if (clientSocket != -1) {
            printf("Connected %s:%d\n", inet_ntoa(remote.sin_addr), ntohs(remote.sin_port));
            clientWorker(clientSocket);
            gClientSocket = 0;
            printf("Disconnected\n");
        } else {
            fprintf(stderr, "Accept failed\n");
        }
    }

    close(listenSocket);

    return NULL;
}

int main()
{
    pthread_t socket_thread;
    if (pthread_create(&socket_thread, NULL, socketLoop, NULL)) {
        error("Error creating thread!\n");
    }

    XRecordContext xrd;
    XRecordRange *range;
    XRecordClientSpec client;
    Display *d0, *d1;

    d0 = XOpenDisplay(NULL);
    d1 = XOpenDisplay(NULL);

    XSynchronize(d0, True);

    if (d0 == NULL || d1 == NULL) {
        error("Cannot connect to X server");
    }

    client = XRecordAllClients;

    range = XRecordAllocRange();
    memset(range, 0, sizeof(XRecordRange));
    range->device_events.first = KeyPress;
    range->device_events.last = KeyRelease;

    xrd = XRecordCreateContext(d0, 0, &client, 1, &range, 1);

    if (!xrd) {
        error("Error in creating context");
    }

    if (!XRecordEnableContext(d1, xrd, eventCallback, NULL)) {
        error("Cound not enable the record context!\n");
    }

    XRecordProcessReplies(d1);

    XRecordDisableContext(d0, xrd);
    XRecordFreeContext(d0, xrd);

    XCloseDisplay(d0);
    XCloseDisplay(d1);

    pthread_join(socket_thread, NULL);

    return EXIT_SUCCESS;
}
