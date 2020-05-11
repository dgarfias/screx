#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <stdlib.h>
#include <stdint.h>
#include <evdi_lib.h>
#include <regex.h>
#include <dirent.h>
#include <rfb/rfb.h>
#include <limits.h>
#include <string.h>

#define MAX_RECTS 16
#define DEFAULT_SKU_AREA_LIMIT 8294400
#define DEFAULT_EDID_NAME "edid.bin"
#define MAX_EDID_SIZE 512
#define TCP_DEFAULT_PORT 5900

// DPMS Mode names
static const char * dpmsModeName[] = {
    "ON MODE", "STANDBY MODE", "SUSPEND MODE", "OFF MODE"
};

// EVDI globals
evdi_handle node;
bool isBufferAllocated = false;
struct evdi_mode currentMode;
struct evdi_buffer buffer;
struct evdi_rect rects[MAX_RECTS];
int tcpPort;

// VNC Globals
rfbScreenInfoPtr rfbScreen;
int connectedClients = 0;

// Fake program arguments
int vncArgc = 1;
char * vncArgv[] = {"screx"};

// VNC Hooks
static void goneClient(rfbClientPtr client) {
  client->clientData = NULL;
  connectedClients--;
}
static enum rfbNewClientAction newClient(rfbClientPtr client) {
  client->clientGoneHook = goneClient;
  connectedClients++;
  return RFB_CLIENT_ACCEPT;
}

// Count card entries
int countEntries() {
  int entries = 0;

  // Regular expression for /dev/dri/cardX
  regex_t regExp;
  regcomp(&regExp, "^card[0-9]*$", REG_NOSUB);

  // Scan through /dev/dri to find card entries
  DIR *dir = opendir("/dev/dri");
  if(dir != NULL) {
    struct dirent *dirEntry;
    while ((dirEntry = readdir(dir))) {
      if(!regexec(&regExp, dirEntry->d_name, 0, NULL, 0)) {
        entries++;
      }
    }
  } else {
    printf("Couldn't open /dev/dri\n");
  }

  return entries++;
}

char * allocateVNCFramebuffer(rfbScreenInfoPtr screen) {
  return buffer.buffer;
}

// Find available node
int findAvailableNode() {
  int availableNode = -1;

  do {
    int cardEntries = countEntries();
    
    for(int i = 0; i < cardEntries; i++) {
      if (evdi_check_device(i) == AVAILABLE) {
        availableNode = i;
        goto end;
      }
    }

    if(!evdi_add_device()) {
      printf("Couldn't create evdi node. Check if evdi module is loaded.\n");
      break;
    }
  } while(availableNode == -1);
  end: ;

  return availableNode;
}

// Open node
evdi_handle openNode() {
  int entryNumber = findAvailableNode();

  if(entryNumber == -1) {
    return EVDI_INVALID_HANDLE;
  }

  return evdi_open(entryNumber);
}

// Close everything
void shutdownEverything(evdi_handle handle) {
  printf("Exiting application...\n");
  if (isBufferAllocated) {
    free(buffer.buffer);
  }
  if (rfbScreen) {
    rfbScreenCleanup(rfbScreen);
    rfbShutdownServer(rfbScreen, true);
  }
  if(handle != EVDI_INVALID_HANDLE) {
    evdi_disconnect(handle);
    evdi_close(handle);
  }
  exit(0);
}

// Adjust pixel format
void adjustPixelFormat(rfbScreenInfoPtr screen) {
  rfbPixelFormat *format = &screen->serverFormat;
  format->redShift = 16;
  format->blueShift = 0;
  printf("Pixel format adjusted\n");
}

// Start VNC Server
rfbScreenInfoPtr startVNCServer() {
  rfbScreenInfoPtr screen = rfbGetScreen(&vncArgc, vncArgv, currentMode.width,
    currentMode.height, 8, 3, currentMode.bits_per_pixel/8);

    char cwd[_PC_PATH_MAX + 1];
    if(getcwd(cwd, sizeof(cwd)) != NULL) {
      screen->httpDir = cwd;
    }

    screen->maxRectsPerUpdate = MAX_RECTS;
    screen->deferUpdateTime = (1e3 / currentMode.refresh_rate) - 1;

  if(!screen) {
    printf("Error getting rfb screen\n");
    return screen;
  }
  adjustPixelFormat(screen);
  rfbPixelFormat *format = &screen->serverFormat;
  printf("Pixel format. Shift -> R: %d G: %d B: %d Max -> R: %d G: %d B: %d\n",
    format->redShift, format->greenShift, format->blueShift, format->redMax,
    format->greenMax, format->blueMax);

    screen->newClientHook = newClient;
    screen->frameBuffer = allocateVNCFramebuffer(screen);
    screen->port = tcpPort;
    screen->ipv6port = screen->port;
    rfbInitServerWithPthreadsAndZRLE(screen);

    return screen;
}

// Mode change Handler
void modeChangedHandler(struct evdi_mode mode, void* userData) {
  printf("Mode changed to %dx%d @ %dHz bpp: %d\n", mode.width, mode.height, 
    mode.refresh_rate, mode.bits_per_pixel);
  currentMode = mode;

  if(isBufferAllocated) {
    free(buffer.buffer);
    evdi_unregister_buffer(node, buffer.id);
  }

  buffer.id = 0;
  buffer.height = mode.height;
  buffer.width = mode.width;
  buffer.stride = mode.bits_per_pixel/8 * mode.width;
  buffer.buffer = malloc(buffer.height * buffer.stride);
  evdi_register_buffer(node, buffer);
  isBufferAllocated = true;
  
  // Register VNC new framebuffer
  if(!rfbScreen) {
    return;
  }

  char *newFramebuffer = allocateVNCFramebuffer(rfbScreen);
  rfbNewFramebuffer(rfbScreen, newFramebuffer, currentMode.width, currentMode.height,
  8, 3, currentMode.bits_per_pixel/8);

  adjustPixelFormat(rfbScreen);
}

// Update Ready Handler
void updateReadyHandler(int buffer, void* userData) {
  int nRects;
  evdi_grab_pixels(node, rects, &nRects);

  for (int i = 0; i < nRects; i++) {
    for (int y = rects[i].y1; y<= rects[i].y2; y++) {
      rfbMarkRectAsModified(rfbScreen, rects[i].x1, rects[i].y1, rects[i].x2, 
        rects[i].y2);
    }
  }
}

// Other handlers (currently not implemented)
void cursorSetHandler(struct evdi_cursor_set cursorSet, void* userData) {
  printf("cursorSetHandler: need to implement!\n");
}
void cursorMoveHandler(struct evdi_cursor_move cursorMove, void* userData) {
  printf("updateMoveHandler: need to implement!\n");
}
void crctStateHandler(int state, void* userData) {
  printf("crctStateHandler: need to implement!\n");
}
void dpmsHandler(int dpmsMode, void* userData) {
  printf("dpmsHandler: need to implement!. %s\n", dpmsModeName[dpmsMode]);

}

// SIGINT handler to exit safely
void signalHandler(int sig) {
  if(sig == SIGINT) {
    shutdownEverything(node);
  }
}

int main(int argc, char const *argv[])
{

  uint32_t skuAreaLimit = DEFAULT_SKU_AREA_LIMIT;
  char * edidFileName = malloc((_PC_NAME_MAX + 1) * sizeof(char));
  strcpy(edidFileName, DEFAULT_EDID_NAME);
  tcpPort = TCP_DEFAULT_PORT;

  // Check program arguments
  for(int i = 1; i < argc; i++) {
    // Help option
    if(strcmp(argv[i], "--help") == 0) {
      printf("Usage: screx [OPTIONS]... \n"
             "Remember to run as superuser and load EVDI module\n\n"
             "  -e,        specify edid file (default is %s)\n"
             "  -p,        specify TCP port (default is %d)\n"
             "  -s,        specify SKU area limit (default is %d\n"
             "  --help     displays this help and exit\n", 
             DEFAULT_EDID_NAME, TCP_DEFAULT_PORT, DEFAULT_SKU_AREA_LIMIT);
        return 1;
    }

    // Edid file argument -e
    if(strcmp(argv[i], "-e") == 0) {
      if(argc >= (i + 2)) {
        strcpy(edidFileName, argv[++i]);
        continue;
      } else {
        printf("screx: Please specify edid file for -e switch");
        return 1;
      }
    }

    // SKU area size limit -s
    if(strcmp(argv[i], "-s") == 0) {
      if(argc >= (i + 2)) {
        skuAreaLimit = (uint32_t) atoi(argv[++i]);
        printf("Specified SKU area limit: %d\n", skuAreaLimit);
        continue;
      } else {
        printf("screx: Please specify sku area limit for -s switch");
        return 1;
      }
    }

    // TCP port
    if(strcmp(argv[i], "-t") == 0) {
      if(argc >= (i + 2)) {
        tcpPort = (int) atoi(argv[++i]);
        continue;
      } else {
        printf("screx: Please specify TCP port for -t switch");
        return 1;
      }
    }

    printf("screx: Invalid option '%s'. Use --help\n", argv[i]);
    return 1;
  }

    // Check if running as superuser
  if (getuid() != 0){
    printf("Please run screx as superuser.\n");
    return 1;
  }

  // Read contents of edid file
  FILE *edidFile;
  long edidSize;
  unsigned char edid[MAX_EDID_SIZE];

  edidFile = fopen(edidFileName, "rb");

  if(edidFile == NULL) {
    printf("Couldn't read edid file %s\n", edidFileName);
    return 1;
  }

  fseek(edidFile, 0, SEEK_END);
  edidSize = ftell(edidFile);

  if(edidSize > MAX_EDID_SIZE) {
    printf("Edid file %s is bigger than max size\n", edidFileName);
    fclose(edidFile);
    return 1;
  }

  rewind(edidFile);
  fread(edid, sizeof(unsigned char), edidSize, edidFile);
  fclose(edidFile);

  // Open EVDI node
  node = openNode();
  if(node == EVDI_INVALID_HANDLE) {
    printf("There was an error opening evdi device node.\n");
    return 1;
  }

  signal(SIGINT, signalHandler);

  // File descriptors for polling
  evdi_selectable nodeFileDescriptor = evdi_get_event_ready(node);

  struct pollfd fds[1];
  fds[0].fd = nodeFileDescriptor;
  fds[0].events = POLLIN;

  // EVDI event handlers
  struct evdi_event_context eventContext;
  eventContext.mode_changed_handler = modeChangedHandler;
  eventContext.crtc_state_handler = crctStateHandler;
  eventContext.cursor_move_handler = cursorMoveHandler;
  eventContext.cursor_set_handler = cursorSetHandler;
  eventContext.update_ready_handler = updateReadyHandler;
  eventContext.dpms_handler = dpmsHandler;

  // Connect to EVDI node
  evdi_connect(node, edid, edidSize, skuAreaLimit);

  // First mode selection
  while (currentMode.width == 0) {
    if (poll(fds, 1, -1)) {
      evdi_handle_events(node, &eventContext);
    }
  }

  rfbScreen = startVNCServer();

  // Check if succeeded to start server
  if (!rfbScreen) {
    printf("Failed to setup VNC server\n");
    shutdownEverything(node);
  }

  printf("Starting event loop..\n");
  while(rfbIsActive(rfbScreen)) {
    // Handle buffer updates
    while(evdi_request_update(node, buffer.id)) {
      updateReadyHandler(buffer.id, NULL);
    }

    // Handle other events
    if(poll(fds, 1, 1)) {
      evdi_handle_events(node, &eventContext);
    }

    rfbProcessEvents(rfbScreen, rfbScreen->deferUpdateTime);
  }
  
  shutdownEverything(node);
  return 0;
  
}
