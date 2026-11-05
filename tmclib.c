#define _GNU_SOURCE

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define ENV_IFACE_PRIMARY "TMC_PRIMARY"
#define ENV_IFACE_SECONDARY "TMC_SECONDARY"
#define TIBIA_GAME_PORT 7171
#define MAX_TRACKED_IPS 128

struct ip_set {
  uint32_t ips[MAX_TRACKED_IPS];
  size_t count;
};

static char iface_primary[IFNAMSIZ];
static char iface_secondary[IFNAMSIZ];

static pthread_mutex_t ip_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct ip_set primary_ips;
static struct ip_set secondary_ips;

static bool ip_set_contains(const struct ip_set *set, uint32_t ip) {
  for (size_t i = 0; i < set->count; ++i) {
    if (set->ips[i] == ip) {
      return true;
    }
  }
  return false;
}

static bool ip_set_insert(struct ip_set *set, uint32_t ip) {
  if (ip_set_contains(set, ip) || set->count >= MAX_TRACKED_IPS) {
    return false;
  }
  set->ips[set->count++] = ip;
  return true;
}

__attribute__((constructor)) static void tmc_init(void) {
  const char *pri = getenv(ENV_IFACE_PRIMARY);
  const char *sec = getenv(ENV_IFACE_SECONDARY);

  if (pri && pri[0] != '\0') {
    snprintf(iface_primary, sizeof(iface_primary), "%s", pri);
  }
  if (sec && sec[0] != '\0') {
    snprintf(iface_secondary, sizeof(iface_secondary), "%s", sec);
  }

  if (iface_primary[0] == '\0' || iface_secondary[0] == '\0') {
    fprintf(stderr,
            "[tmc] warn: pid=%d loaded without both interfaces set "
            "(%s='%s', %s='%s')\n",
            getpid(), ENV_IFACE_PRIMARY, iface_primary, ENV_IFACE_SECONDARY,
            iface_secondary);
    return;
  }

  fprintf(stderr, "[tmc] init: pid=%d primary='%s' secondary='%s'\n", getpid(),
          iface_primary, iface_secondary);
}

static bool bind_socket_to_interface(int sockfd, const char *ifname) {
  char current_dev[IFNAMSIZ] = {0};
  socklen_t len = sizeof(current_dev);

  // Non-blocking sockets may call connect() more than once while polling
  if (getsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, current_dev, &len) == 0 &&
      current_dev[0] != '\0') {
    if (strcmp(current_dev, ifname) != 0) {
      fprintf(
          stderr,
          "[tmc] warn: fd=%d already bound to '%s', cannot rebind to '%s'\n",
          sockfd, current_dev, ifname);
    }
    return false;
  }

  if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, ifname,
                 (socklen_t)(strlen(ifname) + 1)) < 0) {
    fprintf(stderr, "[tmc] error: SO_BINDTODEVICE fd=%d dev='%s' failed: %s\n",
            sockfd, ifname, strerror(errno));
    return false;
  }

  return true;
}

__attribute__((visibility("default"))) int
getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
            struct addrinfo **res) {
  typedef int (*getaddrinfo_fn)(const char *, const char *,
                                const struct addrinfo *, struct addrinfo **);
  static getaddrinfo_fn real_getaddrinfo = NULL;
  if (!real_getaddrinfo) {
    real_getaddrinfo = (getaddrinfo_fn)dlsym(RTLD_NEXT, "getaddrinfo");
  }

  int ret = real_getaddrinfo(node, service, hints, res);
  if (ret != 0 || !node || !res || !*res) {
    return ret;
  }

  bool is_cf = (strstr(node, "prod-cf-") != NULL);
  bool is_lb = (strstr(node, "prod-lb-") != NULL);
  if (!is_cf && !is_lb) {
    return ret;
  }

  pthread_mutex_lock(&ip_mutex);
  for (struct addrinfo *p = *res; p != NULL; p = p->ai_next) {
    if (p->ai_family != AF_INET) {
      continue;
    }
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
    uint32_t addr = ipv4->sin_addr.s_addr;
    bool added = is_cf ? ip_set_insert(&primary_ips, addr)
                       : ip_set_insert(&secondary_ips, addr);
    if (added) {
      char ip_str[INET_ADDRSTRLEN] = {0};
      inet_ntop(AF_INET, &ipv4->sin_addr, ip_str, sizeof(ip_str));
      fprintf(stderr, "[tmc] dns: %s -> %s (%s -> '%s')\n", node, ip_str,
              is_cf ? "primary" : "secondary",
              is_cf ? iface_primary : iface_secondary);
    }
  }
  pthread_mutex_unlock(&ip_mutex);

  return ret;
}

__attribute__((visibility("default"))) int
connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
  typedef int (*connect_fn)(int, const struct sockaddr *, socklen_t);
  static connect_fn real_connect = NULL;
  if (!real_connect) {
    real_connect = (connect_fn)dlsym(RTLD_NEXT, "connect");
  }

  if (!addr || iface_primary[0] == '\0' || iface_secondary[0] == '\0') {
    return real_connect(sockfd, addr, addrlen);
  }

  uint16_t dest_port = 0;
  uint32_t dest_ip4 = 0;

  if (addr->sa_family == AF_INET) {
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    dest_port = ntohs(sin->sin_port);
    dest_ip4 = sin->sin_addr.s_addr;
  } else if (addr->sa_family == AF_INET6) {
    const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)addr;
    dest_port = ntohs(sin6->sin6_port);
    if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
      memcpy(&dest_ip4, &sin6->sin6_addr.s6_addr[12], sizeof(dest_ip4));
    }
  }

  if (dest_port == TIBIA_GAME_PORT) {
    const char *target_iface = NULL;
    const char *route_reason = NULL;

    pthread_mutex_lock(&ip_mutex);
    if (dest_ip4 && ip_set_contains(&primary_ips, dest_ip4)) {
      target_iface = iface_primary;
      route_reason = "primary/cf";
    } else if (dest_ip4 && ip_set_contains(&secondary_ips, dest_ip4)) {
      target_iface = iface_secondary;
      route_reason = "secondary/lb";
    }
    pthread_mutex_unlock(&ip_mutex);

    if (!target_iface) {
      static int conn_toggle = 0;
      bool use_pri = (__sync_fetch_and_add(&conn_toggle, 1) % 2 == 0);
      target_iface = use_pri ? iface_primary : iface_secondary;
      route_reason = use_pri ? "fallback-rr/primary" : "fallback-rr/secondary";
    }

    if (bind_socket_to_interface(sockfd, target_iface)) {
      char ip_str[INET_ADDRSTRLEN] = "ipv6";
      if (dest_ip4) {
        inet_ntop(AF_INET, &dest_ip4, ip_str, sizeof(ip_str));
      }
      fprintf(stderr,
              "[tmc] connect: fd=%d dst=%s:%u route=%s -> bound to '%s'\n",
              sockfd, ip_str, dest_port, route_reason, target_iface);
    }
  }

  return real_connect(sockfd, addr, addrlen);
}
