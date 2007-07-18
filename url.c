#define _GNU_SOURCE	/* strnlen */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <curl/curl.h>

#include "global.h"
#include "file.h"
#include "util.h"
#include "module.h"
#include "net.h"
#include "url.h"

#define CRAMFS_SUPER_MAGIC	0x28cd3d45
#define CRAMFS_SUPER_MAGIC_BIG	0x453dcd28

struct cramfs_super_block {
  unsigned magic;
  unsigned size;
  unsigned flags;
  unsigned future;
  unsigned char signature[16];
  unsigned crc;
  unsigned edition;
  unsigned blocks;
  unsigned files;
  unsigned char name[16];
};

static size_t url_write_cb(void *buffer, size_t size, size_t nmemb, void *userp);
static int url_progress_cb(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow);

static int url_mount_disk(url_t *url, char *dir, int (*test_func)(url_t *));
static int url_progress(url_data_t *url_data);
static int url_setup_device(url_t *url);


void url_read(url_data_t *url_data)
{
  CURL *c_handle;
  int i;
  FILE *f;
  char *buf, *s;
  sighandler_t old_sigpipe = signal(SIGPIPE, SIG_IGN);;

  c_handle = curl_easy_init();
  // fprintf(stderr, "curl handle = %p\n", c_handle);

  // curl_easy_setopt(c_handle, CURLOPT_VERBOSE, 1);

  curl_easy_setopt(c_handle, CURLOPT_WRITEFUNCTION, url_write_cb);
  curl_easy_setopt(c_handle, CURLOPT_WRITEDATA, url_data);
  curl_easy_setopt(c_handle, CURLOPT_ERRORBUFFER, url_data->curl_err_buf);
  curl_easy_setopt(c_handle, CURLOPT_FAILONERROR, 1);
  curl_easy_setopt(c_handle, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(c_handle, CURLOPT_MAXREDIRS, 10);
  curl_easy_setopt(c_handle, CURLOPT_SSL_VERIFYPEER, 0);

  curl_easy_setopt(c_handle, CURLOPT_PROGRESSFUNCTION, url_progress_cb);
  curl_easy_setopt(c_handle, CURLOPT_PROGRESSDATA, url_data);
  curl_easy_setopt(c_handle, CURLOPT_NOPROGRESS, 0);

  url_data->err = curl_easy_setopt(c_handle, CURLOPT_URL, url_data->url->str);
  // fprintf(stderr, "curl opt url = %d\n", url_data->err);

  if(!url_data->err) {
    i = curl_easy_perform(c_handle);
    if(!url_data->err) url_data->err = i;
  }

  if(!url_data->err) {
    url_data->flush = 1;
    url_write_cb(NULL, 0, 0, url_data);
  }

  // fprintf(stderr, "curl perform = %d (%s)\n", url_data->err, url_data->err_buf);

  if(url_data->f) {
    i = url_data->pipe_fd >= 0 ? pclose(url_data->f) : fclose(url_data->f);
    url_data->f = NULL;

    if(url_data->pipe_fd >= 0) {
      i = WIFEXITED(i) ? WEXITSTATUS(i) : 0;
      if(i && i != 2) {
        if(url_data->tmp_file) {
          buf = malloc(url_data->err_buf_len);
          f = fopen(url_data->tmp_file, "r");
          i = fread(buf, 1, url_data->err_buf_len - 1, f);
          fclose(f);
          buf[i] = 0;
          i = strlen(buf) - 1;
          while(i > 0 && isspace(buf[i])) buf[i--] = 0;
          s = buf;
          while(isspace(*s)) s++;
          if(!url_data->err) strcpy(url_data->err_buf, s);
          free(buf);
        }
        url_data->err = 103;
        snprintf(url_data->err_buf, url_data->err_buf_len, "gzip: command terminated");
      }
      // fprintf(stderr, "close = %d\n", i);
    }
    else {
      if(i && !url_data->err) url_data->err = 104;
    }
  }

  /* to get progress bar at 100% when uncompressing */
  url_data->flush = 0;
  url_write_cb(NULL, 0, 0, url_data);

  if(url_data->pipe_fd >= 0) close(url_data->pipe_fd);

  if(url_data->tmp_file) unlink(url_data->tmp_file);

  if(!*url_data->err_buf) {
    memcpy(url_data->err_buf, url_data->curl_err_buf, url_data->err_buf_len);
    *url_data->curl_err_buf = 0;
  }

  curl_easy_cleanup(c_handle);

  signal(SIGPIPE, old_sigpipe);
}


size_t url_write_cb(void *buffer, size_t size, size_t nmemb, void *userp)
{
  url_data_t *url_data = userp;
  size_t z1, z2;
  int i, fd, fd1, fd2, tmp;
  struct cramfs_super_block *cramfs_sb;
  off_t off;

#if 0
  fprintf(stderr,
    "buffer = %p, size = %d, nmemb = %d, userp = %p\n",
    buffer, size, nmemb, userp
  );
#endif

  z1 = size * nmemb;

  if(url_data->buf.len < url_data->buf.max && z1) {
    z2 = url_data->buf.max - url_data->buf.len;
    if(z2 > z1) z2 = z1;
    memcpy(url_data->buf.data + url_data->buf.len, buffer, z2);
    url_data->buf.len += z2;
    z1 -= z2;
    buffer += z2;
  }

  if(
    (url_data->buf.len == url_data->buf.max || url_data->flush) &&
    url_data->buf.len >= 11
  ) {
    if(
      url_data->unzip &&
      url_data->buf.data[0] == 0x1f &&
      url_data->buf.data[1] == 0x8b
    ) {
      url_data->gzip = 1;

      if((url_data->buf.data[3] & 0x08)) {
        i = strnlen((char *) url_data->buf.data + 10, url_data->buf.len - 10);
        if(i < url_data->buf.len - 10) {
          url_data->orig_name = strdup((char *) url_data->buf.data + 10);
        }
      }
    }
    else if(url_data->buf.len > sizeof *cramfs_sb) {
      cramfs_sb = (struct cramfs_super_block *) url_data->buf.data;
      if(
        cramfs_sb->magic == CRAMFS_SUPER_MAGIC ||
        cramfs_sb->magic == CRAMFS_SUPER_MAGIC_BIG
      ) {
        url_data->orig_name = calloc(1, sizeof cramfs_sb->name + 1);
        memcpy(url_data->orig_name, cramfs_sb->name, sizeof cramfs_sb->name);
        url_data->cramfs = 1;
      }
    }

    i = 0;
    if(
      url_data->orig_name &&
      sscanf(url_data->orig_name, "%*s %d", &i) >= 1 &&
      i > 0
    ) {
      url_data->image_size = i;
    }

#if 0
    fprintf(stderr,
      "gzip = %d, cramfs = %d, >%s<\n",
      url_data->gzip, url_data->cramfs, url_data->orig_name
    );
#endif
  }

  if(url_data->buf.len == url_data->buf.max || url_data->flush) {
    if(!url_data->file_opened) {
      url_data->file_opened = 1;
      if(url_data->gzip) {
        url_data->tmp_file = strdup("/tmp/foo_XXXXXX");
        tmp = mkstemp(url_data->tmp_file);
        if(tmp > 0) {
          fd = open(url_data->file_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
          if(fd >= 0) {
            fd1 = dup(1);
            fd2 = dup(2);
            dup2(fd, 1);
            dup2(tmp, 2);
            url_data->pipe_fd = fd;
            url_data->f = popen("gzip -dc", "w");
            dup2(fd1, 1);
            dup2(fd2, 2);
            url_data->zp_total = url_data->image_size << 10;
          }
          else {
            url_data->err = 101;
            snprintf(url_data->err_buf, url_data->err_buf_len, "open: %s: %s", url_data->file_name, strerror(errno));
          }
          close(tmp);
        }
        else {
          url_data->err = 1;
          snprintf(url_data->err_buf, url_data->err_buf_len, "mkstemp: %s", strerror(errno));
        }
      }
      else {
        url_data->f = fopen(url_data->file_name, "w");
        if(!url_data->f) {
          url_data->err = 101;
          snprintf(url_data->err_buf, url_data->err_buf_len, "open: %s: %s", url_data->file_name, strerror(errno));
        }
      }
    }

    if(url_data->f && url_data->buf.len) {
      fwrite(url_data->buf.data, url_data->buf.len, 1, url_data->f);
      url_data->p_now += url_data->buf.len;
    }

    if(url_data->f && z1) {
      fwrite(buffer, z1, 1, url_data->f);
      url_data->p_now += z1;
    }

    if(url_data->buf.max) {
      url_data->buf.len = url_data->buf.max = 0;
      free(url_data->buf.data);
      url_data->buf.data = NULL;
    }
  }

  if(url_data->pipe_fd >= 0) {
    off = lseek(url_data->pipe_fd, 0, SEEK_CUR);
    if(off != -1) url_data->zp_now = off;
  }

  if(url_data->p_total || url_data->zp_total) {
    if(url_data->progress) {
      if(url_data->progress(url_data) && !url_data->err) url_data->err = 102;
    }
  }

  return url_data->err ? 0 : size * nmemb;
}


int url_progress_cb(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow)
{
  url_data_t *url_data = clientp;

  if(!url_data->p_total) url_data->p_total = dltotal;

  return url_data->progress ? url_data->progress(url_data) : 0;
}


/*
 * scheme://domain;user:password@server:port/path?query
 *
 * smb: path = share/path
 * disk: path = [device/]path
 */

url_t *url_set(char *str)
{
  url_t *url = calloc(1, sizeof *url);
  char *s0, *s1, *s2;
  char *tmp = NULL;
  int i;
  unsigned u;
  struct stat sbuf;
  slist_t *sl;

  if(!str) return url;

  url->str = strdup(str);
  s0 = str = strdup(str);

  if((s1 = strchr(s0, ':'))) {
    *s1++ = 0;

    url->scheme = file_sym2num(s0);

    if(url->scheme) {
      s0 = s1;
    
      if(s0[0] == '/' && s0[1] == '/') {
        i = strcspn(s0 + 2, "/?");
        if(i) {
          tmp = strdup(s0 + 2);
          tmp[i] = 0;
        }
        s0 += i + 2;
      }

      if((s1 = strchr(s0, '?'))) {
        *s1++ = 0;
        url->query = slist_split('&', s1);
        for(sl = url->query; sl; sl = sl->next) {
          if((s2 = strchr(sl->key, '='))) {
            *s2++ = 0;
            sl->value = strdup(s2);
          }
        }
      }

      url->path = strdup(*s0 == '/' ? s0 + 1 : s0);
    }
  }
  else {
    // FIXME: should always be 'rel'
    i = file_sym2num(str);
    if(i >= 0) {
      url->scheme = i;
    }
    else if(i == -1) {
      url->scheme = inst_rel;
      url->path = strdup(str);
    }
  }

  free(str);

  if(tmp) {
    s0 = tmp;

    if((s1 = strchr(s0, ';'))) {
      *s1++ = 0;
      url->domain = strdup(s0);
      s0 = s1;
    }

    if((s1 = strchr(s0, '@'))) {
      *s1++ = 0;
      if((s2 = strchr(s0, ':'))) {
        *s2++ = 0;
        url->password = strdup(s2);
      }
      url->user = strdup(s0);
      s0 = s1;
    }

    if((s1 = strchr(s0, ':'))) {
      *s1++ = 0;
      if(*s1) {
        u = strtoul(s1, &s1, 0);
        if(!*s1) url->port = u;
      }
    }

    url->server = strdup(s0);

    free(tmp);
    tmp = NULL;
  }

  /* smb: first path element is share */
  if(url->scheme == inst_smb && url->path) {
    url->share = url->path;
    url->path = NULL;

    if((s1 = strchr(url->share, '/'))) {
      *s1++ = 0;
      url->path = strdup(s1);
    }
  }  

  /* unescape strings */
  {
    char **str[] = {
      &url->server, &url->share, &url->path, &url->user,
      &url->password, &url->domain
    };

    for(i = 0; i < sizeof str / sizeof *str; i++) {
      if(*str[i]) {
        s0 = *str[i];
        *str[i] = curl_easy_unescape(NULL, s0, 0, NULL);
        free(s0);
      }
    }
  }

  /* disk/cdrom: allow path to begin with device name */
  if(
    (
      url->scheme == inst_disk ||
      url->scheme == inst_cdrom ||
      url->scheme == inst_dvd ||
      url->scheme == inst_floppy ||
      url->scheme == inst_hd
    ) && url->path
  ) {
    tmp = malloc(strlen(url->path) + 6);
    strcpy(tmp, "/");

    if(
      strncmp(url->path, "dev", 3) ||
      (url->path[3] != 0 && url->path[3] != '/')
    ) {
      strcat(tmp, "dev/");
    }

    strcat(tmp, url->path);

    s0 = tmp;
    do {
      if((s0 = strchr(s0 + 1, '/'))) *s0 = 0;

      if(stat(tmp, &sbuf)) break;
      if(S_ISBLK(sbuf.st_mode)) {
        url->device = strdup(short_dev(tmp));
        free(url->path);
        url->path = s0 ? strdup(s0 + 1) : NULL;
      }
      if(!S_ISDIR(sbuf.st_mode)) break;
    }
    while(s0 && (*s0 = '/'));

    free(tmp);
    tmp = NULL;
  }

  if((sl = slist_getentry(url->query, "device"))) {
    s0 = short_dev(sl->value);
    str_copy(&url->device, *s0 ? s0 : NULL);
  }

  if((sl = slist_getentry(url->query, "instsys"))) {
    str_copy(&url->instsys, sl->value);
  }

  if((sl = slist_getentry(url->query, "proxy"))) {
    str_copy(&url->proxy, sl->value);
  }

  if(
    url->scheme == inst_file ||
    url->scheme == inst_nfs ||
    url->scheme == inst_smb ||
    url->scheme == inst_cdrom ||
    url->scheme == inst_floppy ||
    url->scheme == inst_hd ||
    url->scheme == inst_disk ||
    url->scheme == inst_dvd ||
    url->scheme == inst_exec
    ) {
    url->is.mountable = 1;
  }

  if(
    url->scheme == inst_nfs ||
    url->scheme == inst_ftp ||
    url->scheme == inst_smb ||
    url->scheme == inst_http ||
    url->scheme == inst_tftp
    ) {
    url->is.network = 1;
  }

  if(
    url->scheme == inst_cdrom ||
    url->scheme == inst_dvd
    ) {
    url->is.cdrom = 1;
  }

  /* ensure leading "/" if mountable */
  if(url->is.mountable) {
    if(url->path) {
      if(*url->path != '/') {
        strprintf(&url->path, "/%s", url->path);
      }
    }
    else {
      url->path = strdup("/");
    }
  }

  fprintf(stderr, "url = %s\n", url->str);
  fprintf(stderr, "  scheme = %s", get_instmode_name(url->scheme));
  if(url->server) fprintf(stderr, ", server = \"%s\"", url->server);
  if(url->port) fprintf(stderr, ", port = %u", url->port);
  if(url->path) fprintf(stderr, ", path = \"%s\"", url->path);
  fprintf(stderr, "\n");

  if(url->user || url->password) {
    i = 0;
    if(url->user) fprintf(stderr, "%c user = \"%s\"", i++ ? ',' : ' ', url->user);
    if(url->password) fprintf(stderr, "%c password = \"%s\"", i++ ? ',' : ' ', url->password);
    fprintf(stderr, "\n");
  }

  if(url->share || url->domain || url->device) {
    i = 0;
    if(url->share) fprintf(stderr, "%c share = \"%s\"", i++ ? ',' : ' ', url->share);
    if(url->domain) fprintf(stderr, "%c domain = \"%s\"", i++ ? ',' : ' ', url->domain);
    if(url->device) fprintf(stderr, "%c device = \"%s\"", i++ ? ',' : ' ', url->device);
    fprintf(stderr, "\n");
  }

  fprintf(stderr, "  network = %u, mountable = %u\n", url->is.network, url->is.mountable);

  if(url->instsys) fprintf(stderr, "  instsys = %s\n", url->instsys);

  if(url->proxy) fprintf(stderr, "  proxy = %s\n", url->proxy);

  if(url->query) {
    fprintf(stderr, "  query:\n");
    for(sl = url->query; sl; sl = sl->next) {
      fprintf(stderr, "    %s = \"%s\"\n", sl->key, sl->value);
    }
  }

  return url;
}


url_t *url_free(url_t *url)
{
  if(url) {
    free(url->str);
    free(url->server);
    free(url->share);
    free(url->path);
    free(url->user);
    free(url->password);
    free(url->domain);
    free(url->device);
    free(url->proxy);
    free(url->mount);
    free(url->tmp_mount);

    free(url->used.device);
    free(url->used.hwaddr);
    free(url->used.model);
    free(url->used.server.name);

    slist_free(url->query);

    free(url);
  }

  return NULL;
}


url_data_t *url_data_new()
{
  static int curl_init = 0;
  int err;
  url_data_t *url_data = calloc(1, sizeof *url_data);

  url_data->err_buf_len = CURL_ERROR_SIZE;
  *(url_data->err_buf = malloc(CURL_ERROR_SIZE)) = 0;
  *(url_data->curl_err_buf = malloc(CURL_ERROR_SIZE)) = 0;

  url_data->buf.data = malloc(url_data->buf.max = 256);
  url_data->buf.len = 0;

  url_data->pipe_fd = -1;
  url_data->percent = -1;

  if(!curl_init) {
    curl_init = 1;
    err = curl_global_init(CURL_GLOBAL_ALL);
    if(err) fprintf(stderr, "curl init = %d\n", err);
  }

  return url_data;
}


void url_data_free(url_data_t *url_data)
{
  url_free(url_data->url);

  free(url_data->file_name);
  free(url_data->err_buf);
  free(url_data->curl_err_buf);
  free(url_data->orig_name);
  free(url_data->tmp_file);
  free(url_data->buf.data);
  free(url_data->label);

  free(url_data);
}


void url_cleanup()
{
  curl_global_cleanup();
}


/*
 * default progress indicator
 */
int url_progress(url_data_t *url_data)
{
  int percent = -1;
  char *buf = NULL;

  if(url_data->p_total) {
    percent = (100 * (uint64_t) url_data->p_now) / url_data->p_total;
  }
  else if(url_data->zp_total) {
    percent = (100 * (uint64_t) url_data->zp_now) / url_data->zp_total;
  }

  if(percent > 100) percent = 100;

  if(!url_data->label_shown) {
    char *label = NULL;

    str_copy(&label, url_data->label);

    if(!label) strprintf(&label, "Loading %s", url_print(url_data->url, 0));

    if(percent >= 0) {
      strprintf(&buf,
        "%s (%u kB) -     ",
        label,
        ((url_data->zp_total ?: url_data->p_total) + 1023) >> 10
      );
    }
    else {
      strprintf(&buf, "%s -          ", label);
    }
    printf("%s", buf);
    url_data->label_shown = 1;

    str_copy(&label, NULL);
  }

  if(percent >= 0) {
    if(percent != url_data->percent) {
      printf("\x08\x08\x08\x08%3d%%", percent);
      url_data->percent = percent;
    }
  }
  else {
    percent = (url_data->zp_now ?: url_data->p_now) >> 10;
    if(percent > url_data->percent + 100 || url_data->flush) {
      printf("\x08\x08\x08\x08\x08\x08\x08\x08\x08%6u kB", percent);
      url_data->percent = percent;
    }
  }

  fflush(stdout);

  str_copy(&buf, NULL);

  return 0;
}


/*
 * Unmounts volumes used by 'url'.
 */
void url_umount(url_t *url)
{
  if(!url) return;

  if(!util_umount(url->mount)) {
    str_copy(&url->mount, NULL);
  }
  if(util_umount(url->tmp_mount)) {
    str_copy(&url->tmp_mount, NULL);
  }
}


/*
 * Mount url to dir; if dir is NULL, assign temporary mountpoint.
 *
 * If test_func() is set it must return:
 *   0: failed
 *   1: ok
 *   2: ok, but continue search
 *
 * url->used.device must be set
 * 
 * return:
 *   0: failed
 *   1: ok
 *   2: ok, but continue search
 *
 * *** Note: inverse return value compared to url_mount(). ***
 */
int url_mount_disk(url_t *url, char *dir, int (*test_func)(url_t *))
{
  int ok = 0, file_type, err = 0;
  char *path = NULL, *buf = NULL, *s;
  url_data_t *url_data;

  fprintf(stderr, "url mount: trying %s\n", url_print(url, 0));
  if(url->used.model) fprintf(stderr, "model: %s\n", url->used.model);

  if(
    !url ||
    !url->scheme ||
    !url->path ||
    (!url->used.device && url->scheme != inst_file)
  ) return 0;

  url_umount(url);
  str_copy(&url->tmp_mount, NULL);
  str_copy(&url->mount, NULL);

  if(!url_setup_device(url)) return 0;

  if(!url->is.network) {
    /* local device */

    /* we might need an extra mountpoint */
    if(url->scheme != inst_file && strcmp(url->path, "/")) {
      str_copy(&url->tmp_mount, new_mountpoint());
      ok = util_mount_ro(url->used.device, url->tmp_mount) ? 0 : 1;

      if(!ok) {
        fprintf(stderr, "disk: %s: mount failed\n", url->used.device);
        str_copy(&url->tmp_mount, NULL);

        return ok;
      }
    }

    if(url->scheme == inst_file) {
      str_copy(&path, url->path);
    }
    else if(url->tmp_mount) {
      strprintf(&path, "%s%s", url->tmp_mount, url->path);
    }
    else {
      str_copy(&path, url->used.device);
    }
  }
  else {
    /* network device */

    switch(url->scheme) {
      case inst_nfs:
        str_copy(&url->mount, dir ?: new_mountpoint());
        err = net_mount_nfs(url->mount, &url->used.server, url->path);
        fprintf(stderr, "nfs: %s -> %s (%d)\n", url->path, url->mount, err);
        if(err == ENOTDIR || err == ENOENT) {
          str_copy(&url->mount, NULL);
          str_copy(&buf, url->path);

          if((s = strrchr(buf, '/')) && s != buf && s[1]) {
            *s++ = 0;
            str_copy(&url->tmp_mount, new_mountpoint());
            err = net_mount_nfs(url->tmp_mount, &url->used.server, buf);
            if(err) {
              fprintf(stderr, "nfs: %s: mount failed\n", url->used.device);
              str_copy(&url->tmp_mount, NULL);
            }
            else {
              strprintf(&path, "%s/%s", url->tmp_mount, s);
            }
          }

          str_copy(&buf, NULL);
        }
        else if(!err) {
          str_copy(&path, url->mount);
        }
        break;

      case inst_smb:
        if(strcmp(url->path, "/")) {
          str_copy(&url->tmp_mount, new_mountpoint());
          s = url->tmp_mount;
        }
        else {
          str_copy(&url->mount, dir ?: new_mountpoint());
          s = url->mount;
        }
        err = net_mount_smb(s, &url->used.server, url->share, url->user, url->password, url->domain);
        fprintf(stderr, "smb: %s -> %s (%d)\n", url->share, s, err);
        if(err) {
          str_copy(&url->tmp_mount, NULL);
          str_copy(&url->mount, NULL);
        }
        else {
          if(url->mount) {
            str_copy(&path, url->mount);
          }
          else {
            strprintf(&path, "%s%s", url->tmp_mount, url->path);
          }
        }
        break;

      case inst_http:
      case inst_ftp:
        break;

      default:
        fprintf(stderr, "%s: unsupported scheme\n", get_instmode_name(url->scheme));
        err = 1;
        break;
    }
  }

  if(!err) {
    if(url->is.mountable) {
      file_type = util_check_exist(path);

      if(file_type && (file_type == 'r' || file_type == 'b')) url->is.file = 1;

      if(file_type) {
        if(url->is.file && (url->download || !util_is_mountable(path))) {
          str_copy(&url->mount, dir ?: new_mountpoint());
          url_data = url_data_new();
          url_data->unzip = 1;
          strprintf(&buf, "file:%s", path);
          url_data->url = url_set(buf);
          url_data->file_name = strdup(new_download());
          url_data->progress = url_progress;
          strprintf(&url_data->label, "Loading %s", url_print(url, 0));
          fprintf(stderr, "loading %s -> %s\n", url_print(url, 0), url_data->file_name);
          url_read(url_data);
          printf("\n"); fflush(stdout);

          if(url_data->err) {
            fprintf(stderr, "error %d: %s\n", url_data->err, url_data->err_buf);
          }
          else {
            ok = util_mount_ro(url_data->file_name, url->mount) ? 0 : 1;
          }
          if(!ok) unlink(url_data->file_name);

          url_data_free(url_data);
          str_copy(&buf, NULL);
        }
        else {
          if(!url->mount) {
            str_copy(&url->mount, dir ?: new_mountpoint());
            ok = util_mount_ro(path, url->mount) ? 0 : 1;
          }
          else {
            ok = 1;
          }
        }
      }
      else {
        ok = 0;
      }
    }
    else {
      ok = 1;
    }
  }

  if(ok && test_func && !(ok = test_func(url))) {
    fprintf(stderr, "disk: mount ok but test failed\n");
  }

  if(!ok) {
    fprintf(stderr, "url mount: %s failed\n", url_print(url, 0));

    util_umount(url->mount);
    util_umount(url->tmp_mount);

    str_copy(&url->tmp_mount, NULL);
    str_copy(&url->mount, NULL);
  }
  else {
    fprintf(stderr, "url mount: %s @ %s\n", url_print(url, 0), url->mount);
  }

  str_copy(&path, NULL);

  return ok;
}


/*
 * Mount url to dir; if dir is NULL, assign temporary mountpoint.
 *
 * If url->used.device is not set, try all appropriate devices.
 *
 * return:
 *   0: ok
 *   1: failed
 *   sets url->used.device, url->mount, url->tmp_mount (if ok)
 *
 * *** Note: inverse return value compared to url_mount_disk(). ***
 */
int url_mount(url_t *url, char *dir, int (*test_func)(url_t *))
{
  int err = 0, ok, found;
  hd_t *hd;
  hd_res_t *res;
  char *hwaddr;
  hd_hw_item_t hw_item = hw_network_ctrl;

  if(!url || !url->scheme) return 1;

  update_device_list(0);

  if(!config.hd_data) return 1;

  if(
    url->scheme == inst_file ||
    url->used.device
  ) {
    return url_mount_disk(url, dir, test_func) ? 0 : 1;
  }

  if(!url->is.network) {
    switch(url->scheme) {
      case inst_cdrom:
        hw_item = hw_cdrom;
        break;

      case inst_floppy:
        hw_item = hw_floppy;
        break;

      default:
        hw_item = hw_block;
        break;
    }
  }

  for(found = 0, hd = hd_list(config.hd_data, hw_item, 0, NULL); hd; hd = hd->next) {
    for(hwaddr = NULL, res = hd->res; res; res = res->next) {
      if(res->any.type == res_hwaddr) {
        hwaddr = res->hwaddr.addr;
        break;
      }
    }

    if(
      (
        url->scheme == inst_hd &&
        (					/* hd means: */
          hd_is_hw_class(hd, hw_floppy) ||	/*  - not a floppy */
          hd_is_hw_class(hd, hw_cdrom) ||	/*  - not a cdrom */
          hd->child_ids				/*  - has no partitions */
        )
      ) ||
      !hd->unix_dev_name
    ) continue;

    if(
      url->device &&
      !match_netdevice(short_dev(hd->unix_dev_name), hwaddr, url->device) &&
      !search_str_list(hd->unix_dev_names, long_dev(url->device))
    ) continue;

    str_copy(&url->used.device, hd->unix_dev_name);
    str_copy(&url->used.model, hd->model);
    str_copy(&url->used.hwaddr, hwaddr);

    url->is.wlan = hd->is.wlan;

    if((ok = url_mount_disk(url, dir, test_func))) {
      found++;
      if(hd_is_hw_class(hd, hw_cdrom)) url->is.cdrom = 1;
      if(ok == 1) break;
    }
    else {
      err = 1;
    }
  }

  /* should not happen, but anyway: device name was not in our list */
  if(!err && !found && !url->used.device && url->device) {
    str_copy(&url->used.device, long_dev(url->device));
    str_copy(&url->used.model, NULL);
    str_copy(&url->used.hwaddr, NULL);
    err = url_mount_disk(url, dir, test_func) ? 0 : 1;
  }

  if(err) {
    str_copy(&url->used.device, NULL);
    str_copy(&url->used.model, NULL);
    str_copy(&url->used.hwaddr, NULL);
  }

  return found ? 0 : err;
}


/*
 * Read file 'src' relative to 'url' and write it to 'dst'. If 'dir' is set,
 * mount 'url' at 'dir' if necessary.
 *
 * return:
 *   0: ok
 *   1: failed
 */
int url_read_file(url_t *url, char *dir, char *src, char *dst, char *label, unsigned flags)
{
  int err = 0;
  char *buf1 = NULL;

  int test_and_copy(url_t *url)
  {
    int ok = 0, new_url = 0;
    char *old_path, *buf = NULL;
    url_data_t *url_data;

    if(!url) return 0;

    if(url->is.mountable && url->scheme != inst_file) {
      if(!url->mount) return 0;
      ok = util_check_exist2(url->mount, src) == 'r' ? 1 : 0;
      if(!ok) return ok;
      strprintf(&buf, "file:%s", url->mount);
      url = url_set(buf);
      new_url = 1;
    }

    url_data = url_data_new();

    old_path = url->path;
    url->path = NULL;

    strprintf(&url->path, "%s/%s", old_path, *src == '/' ? src + 1 : src);
    str_copy(&buf, url_print(url, 1));
    url_data->url = url_set(buf);

    free(url->path);
    url->path = old_path;

    url_data->file_name = strdup(dst);

    if((flags & URL_FLAG_UNZIP)) url_data->unzip = 1;
    if((flags & URL_FLAG_PROGRESS)) url_data->progress = url_progress;
    str_copy(&url_data->label, label);

    fprintf(stderr, "loading %s -> %s\n", url_print(url_data->url, 0), url_data->file_name);

    url_read(url_data);
    if(url_data->progress) printf("\n");
    fflush(stdout);

    if(url_data->err) {
      if(url_data->progress) {
        printf("error %d: %s\n", url_data->err, url_data->err_buf);
        fflush(stdout);
      }
      fprintf(stderr, "error %d: %s\n", url_data->err, url_data->err_buf);
    }
    else {
      ok = 1;
    }

    str_copy(&buf, NULL);

    if(new_url) url_free(url);

    url_data_free(url_data);

    return ok;
  }

  if(!src || !dst) return 1;

  if(url->mount) {
    strprintf(&buf1, "file:%s", url->mount);
    url = url_set(buf1);
    err = test_and_copy(url) ? 0 : 1;
    str_copy(&buf1, NULL);
    url_free(url);
  }
  else {
    if(url->is.mountable) {
      err = url_mount(url, dir, test_and_copy);
    }
    else {
      err = test_and_copy(url) ? 0 : 1;
    }
  }

  return err;
}


/*
 * Find repository (and mount at 'dir' if possbile).
 * Mount instsys, too, if it is a relative url.
 *
 * return:
 *   0: ok
 *   1: failed
 */
int url_find_repo(url_t *url, char *dir)
{
  int err = 0;

  /*
   * 0: failed, 1: ok, 2: ok but continue search
   */
  int test_is_repo(url_t *url)
  {
    int ok = 0;
    char *buf = NULL, *file_name;

    if(
      !url ||
      (!url->mount && url->is.mountable) ||
      !config.url.instsys ||
      !config.url.instsys->scheme
    ) return 0;

    if(url_read_file(url, NULL, "/content", "/tmp/content", NULL, URL_FLAG_PROGRESS)) return 0;

    if(config.url.instsys->scheme != inst_rel) return 1;

    if(
      url->is.mountable &&
      !util_check_exist2(url->mount, config.url.instsys->path)
    ) return 0;

    if(url->is.mountable) strprintf(&buf, "%s/%s", url->mount, config.url.instsys->path);

    if(
      !config.download.instsys &&
      !config.rescue &&
      url->is.mountable &&
      util_is_mountable(buf)
    ) {
      ok = util_mount_ro(buf, config.mountpoint.instsys) ? 0 : 1;
    }
    else {
      if(!url_read_file(url,
        NULL,
        config.url.instsys->path,
        file_name = strdup(new_download()),
        txt_get(config.rescue ? TXT_LOADING_RESCUE : TXT_LOADING_INSTSYS),
        URL_FLAG_PROGRESS + URL_FLAG_UNZIP
      )) {
        ok = util_mount_ro(file_name, config.mountpoint.instsys) ? 0 : 1;
      }

      free(file_name);
    }

    str_copy(&buf, NULL);

    if(ok) str_copy(&config.url.instsys->mount, config.mountpoint.instsys);

    return ok;
  }

  fprintf(stderr, "repository: looking for %s\n", url_print(url, 0));

  err = url_mount(url, dir, test_is_repo);

  if(err) {
    fprintf(stderr, "repository: not found\n");
  }
  else {
    fprintf(stderr, "repository: using %s @ %s\n", url_print(url, 0), url->mount);
  }

  return err;
}


/*
 * Find instsys (and mount at 'dir' if possbile).
 *
 * return:
 *   0: ok
 *   1: failed
 */
int url_find_instsys(url_t *url, char *dir)
{
  int err = 0;

  if(
    !url ||
    !url->scheme ||
    url->scheme == inst_rel ||
    !url->path
  ) return 1;

  if(!url->is.mountable) return 1;

  if(config.download.instsys || config.rescue) url->download = 1;
  err = url_mount(url, dir, NULL);

  return err;
}


/*
 * Print url to string.
 *
 * scheme://domain;user:password@server:port/path?query
 */
char *url_print(url_t *url, int format)
{
  static char *buf = NULL, *s;
  int q = 0;

  str_copy(&buf, NULL);
  if(!url) return buf;

  strprintf(&buf, "%s:", get_instmode_name(url->scheme));

  if(url->domain || url->user || url->password || url->server || url->port) {
    strprintf(&buf, "%s//", buf);
    if(url->domain) strprintf(&buf, "%s%s;", buf, url->domain);
    if(url->user) {
      s = curl_easy_escape(NULL, url->user, 0);
      strprintf(&buf, "%s%s", buf, s);
      curl_free(s);
    }
    if(url->password) {
      if(format == 0) {
        strprintf(&buf, "%s:***", buf);
      }
      else {
        s = curl_easy_escape(NULL, url->password, 0);
        strprintf(&buf, "%s:%s", buf, s);
        curl_free(s);
      }
    }
    if(url->user || url->password) strprintf(&buf, "%s@", buf);
    if(url->server) strprintf(&buf, "%s%s", buf, url->server);
    if(url->port) strprintf(&buf, "%s:%u", buf, url->port);
  }

  if(url->share) strprintf(&buf, "%s/%s", buf, url->share);
  if(url->path) {
    strprintf(&buf, "%s/%s%s",
      buf,
      url->scheme == inst_ftp && *url->path == '/' ? "%2F" : "",
      *url->path == '/' ? url->path + 1 : url->path
    );
  }

  if(format == 0) {
    if(url->used.device) {
      strprintf(&buf, "%s%cdevice=%s", buf, q++ ? '&' : '?', short_dev(url->used.device));
    }

    if(url->used.hwaddr) {
      strprintf(&buf, "%s%chwaddr=%s", buf, q++ ? '&' : '?', url->used.hwaddr);
    }
  }

  return buf;
}


/*
 * Load fs module or setup network interface.
 *
 * return:
 *   0: failed
 *   1: ok
 */
int url_setup_device(url_t *url)
{
  int ok = 0, i;
  char *module, *type, *s;

  if(!url) return 0;

  if(url->scheme == inst_file) return 1;

  if(!url->used.device) return 0;

  if(!url->is.network) {
    /* load fs module if necessary */

    type = util_fstype(url->used.device, &module);
    if(module) mod_modprobe(module, NULL);
    if(type && strcmp(type, "swap")) ok = 1;
  }
  else {
    /* setup interface */

    if(
      config.net.configured &&
      config.net.device &&
      !strcmp(url->used.device, config.net.device)
    ) {
      return 1;
    }

    if(
      !strncmp(url->used.device, "lo", sizeof "lo" - 1) ||
      !strncmp(url->used.device, "sit", sizeof "sit" - 1)
    ) return 0;

    /* if(!getenv("PXEBOOT")) */ net_stop();

    config.net.configured = nc_none;
    str_copy(&config.net.device, NULL);

    fprintf(stderr, "interface setup: %s\n", url->used.device);

    if(url->is.wlan && wlan_setup()) return 0;

    config.net.configured = nc_static;

    /* we need at least ip & netmask for static network config */
    if((net_config_mask() & 3) != 3) {
      printf(
        "Sending %s request to %s...\n",
        config.net.use_dhcp ? "DHCP" : "BOOTP",
        url->used.device
      );
      fflush(stdout);
      fprintf(stderr,
        "sending %s request to %s... ",
        config.net.use_dhcp ? "DHCP" : "BOOTP",
        url->used.device
      );

      config.net.use_dhcp ? net_dhcp() : net_bootp();

      if(
        !config.test &&
        (
          !config.net.hostname.ok ||
          !config.net.netmask.ok ||
          !config.net.broadcast.ok
        )
      ) {
        fprintf(stderr, "no/incomplete answer.\n");
        config.net.configured = nc_none;

        return 0;
      }
      fprintf(stderr, "ok.\n");

      config.net.configured = config.net.use_dhcp ? nc_dhcp : nc_bootp;
    }

    if(net_activate_ns()) {
      fprintf(stderr, "network setup failed\n");
      config.net.configured = nc_none;

      return 0;
    }
    else {
      fprintf(stderr, "%s activated\n", url->used.device);
    }

    str_copy(&config.net.device, url->used.device);

#if 0
    while(config.instmode == inst_slp) {
      extern int slp_get_install(void);
      if(slp_get_install()) {
        fprintf(stderr, "SLP failed\n");

        return 0;
      }
    }
#endif

    net_ask_password();

    name2inet(&url->used.server, url->server);

    if(net_check_address2(&url->used.server, 1)) {
      fprintf(stderr, "invalid server address: %s\n", url->used.server.name);
      config.net.configured = nc_none;

      return 0;
    }

    s = inet2print(&config.net.hostname);
    fprintf(stderr, "hostip: %s/", *s ? s : "<no ip>");
    s = inet2print(&config.net.netmask);
    fprintf(stderr, "%s\n", *s ? s : "<no netmask>");
    if(config.net.gateway.ok) {
      fprintf(stderr, "gateway: %s\n", inet2print(&config.net.gateway));
    }
    for(i = 0; i < sizeof config.net.nameserver / sizeof *config.net.nameserver; i++) {
      if(config.net.nameserver[i].ok) {
        fprintf(stderr, "nameserver %d: %s\n", i, inet2print(&config.net.nameserver[i]));
      }
    }

    ok = 1;
  }


  return ok;
}


