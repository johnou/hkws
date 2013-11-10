#include <stdio.h>

#include <stdbool.h>
#include <pthread.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/keysymdef.h>
#include <X11/XKBlib.h>

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

void swallow_keystroke(Display * dpy, XEvent * ev)
{
    XAllowEvents(dpy, AsyncKeyboard, ev->xkey.time);
    /* burp */
}

void passthru_keystroke(Display * dpy, XEvent * ev)
{
    /* pass it through to the app, as if we never intercepted it */
    XAllowEvents(dpy, ReplayKeyboard, ev->xkey.time);
    XFlush(dpy); /* don't forget! */
}

void error(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

int safeSend(int clientSocket, const uint8_t *buffer, size_t bufferSize)
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

void *eventLoop(void *ptr)
{
    Window root;
    Display * dpy = XOpenDisplay(0x0);
    KeyCode super;
    XEvent event;

    size_t frameSize = BUF_LEN;
    uint8_t buffer[BUF_LEN];
    uint8_t payload[12];

    if (!dpy) return NULL;

    root = DefaultRootWindow(dpy);

    XAllowEvents(dpy, AsyncKeyboard, CurrentTime);
    XkbSetDetectableAutoRepeat(dpy, true, NULL);

    super = XKeysymToKeycode(dpy, XStringToKeysym("Super_L"));

    XGrabKey(dpy, super, AnyModifier, root, true, GrabModeAsync, GrabModeAsync); // GrabModeSync allows for passthrough but does not fire KeyRelease

    for (;;)
    {
        XNextEvent(dpy, &event);
        if (gClientSocket != 0) {
             frameSize = BUF_LEN;
             memset(buffer, 0, BUF_LEN);
             frameSize = sprintf((char *)payload, "{\"keycode\" : %d, \"type\" : %d}", event.xkey.keycode, event.type);
             wsMakeFrame(payload, frameSize, buffer, &frameSize, WS_TEXT_FRAME);
             safeSend(gClientSocket, buffer, frameSize);
        }
        //passthru_keystroke(dpy, &event);
    }

    XUngrabKey(dpy, super, 0, root);
    XCloseDisplay(dpy);

    return NULL;
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
        ssize_t readed = recv(clientSocket, buffer+readedLength, BUF_LEN-readedLength, 0);
        if (!readed) {
            close(clientSocket);
            perror("Recv failed\n");
            return;
        }
        readedLength+= readed;
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
            } else if (frameType == WS_TEXT_FRAME) {
                uint8_t *receivedString = NULL;
                receivedString = malloc(dataSize+1);
                assert(receivedString);
                memcpy(receivedString, data, dataSize);
                receivedString[ dataSize ] = 0;
                
                prepareBuffer;
                //wsMakeFrame(receivedString, dataSize, buffer, &frameSize, WS_TEXT_FRAME);
                //if (safeSend(clientSocket, buffer, frameSize) == EXIT_FAILURE)
                //    break;
                initNewFrame;
            }
        }
    } // read/write cycle
    
    close(clientSocket);
}

int main(void)
{
    pthread_t event_thread;

    if (pthread_create(&event_thread, NULL, eventLoop, NULL)) {
        error("Error creating thread!\n");
    }

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
        int clientSocket = accept(listenSocket, (struct sockaddr*)&remote, &sockaddrLen);
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

    if (pthread_join(event_thread, NULL)) {
        error("Error joining thread!\n");
    }

    return EXIT_SUCCESS;

}
