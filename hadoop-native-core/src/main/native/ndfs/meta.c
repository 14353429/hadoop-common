/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "common/hadoop_err.h"
#include "common/hconf.h"
#include "common/net.h"
#include "common/string.h"
#include "common/uri.h"
#include "fs/common.h"
#include "fs/fs.h"
#include "ndfs/meta.h"
#include "ndfs/permission.h"
#include "ndfs/util.h"
#include "protobuf/ClientNamenodeProtocol.call.h"
#include "protobuf/hdfs.pb-c.h.s"
#include "rpc/messenger.h"
#include "rpc/proxy.h"

#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <uv.h>

#define DEFAULT_NN_PORT 8020

#define FS_PERMISSIONS_UMASK_KEY "fs.permissions.umask-mode"

#define FS_PERMISSIONS_UMASK_DEFAULT "022"

#define DFS_CLIENT_WRITE_EXCLUDE_NODES_CACHE_EXPIRY_INTERVAL \
    "dfs.client.write.exclude.nodes.cache.expiry.interval.millis"

#define DFS_CLIENT_WRITE_EXCLUDE_NODES_CACHE_EXPIRY_INTERVAL_DEFAULT \
    (10LL * 60LL * 1000LL)

/** Whole-filesystem stats sent back from the NameNode. */
struct hadoop_vfs_stats {
    int64_t capacity;
    int64_t used;
    int64_t remaining;
    int64_t under_replicated;
    int64_t corrupt_blocks;
    int64_t missing_blocks;
};

/** Server defaults sent back from the NameNode. */
struct ndfs_server_defaults {
    uint64_t blocksize;
};

static struct hadoop_err *ndfs_connect_setup_conf(struct native_fs *fs,
                                                  struct hconf *hconf);

static struct hadoop_err *populate_file_info(struct file_info *info,
                    HdfsFileStatusProto *status, const char *prefix);

static void ndfs_free(struct native_fs *fs);

static struct hadoop_err *ndfs_get_server_defaults(struct native_fs *fs,
            struct ndfs_server_defaults *defaults)
{
    struct hadoop_err *err = NULL;
    GetServerDefaultsRequestProto req =
        GET_SERVER_DEFAULTS_REQUEST_PROTO__INIT;
    GetServerDefaultsResponseProto *resp = NULL;
    struct hrpc_proxy proxy;

    ndfs_nn_proxy_init(fs, &proxy);
    err = cnn_get_server_defaults(&proxy, &req, &resp);
    if (err) {
        goto done;
    }
    defaults->blocksize = resp->serverdefaults->blocksize;

done:
    if (resp) {
        get_server_defaults_response_proto__free_unpacked(resp, NULL);
    }
    return err;
}

/**
 * Parse an address in the form <hostname> or <hostname>:<port>.
 *
 * @param host          The hostname
 * @param addr          (out param) The sockaddr.
 * @param default_port  The default port to use, if one is not found in the
 *                          string.
 *
 * @return              NULL on success; the error otherwise.
 */
static struct hadoop_err *parse_rpc_addr(const char *input,
            struct sockaddr_in *out, int default_port)
{
    struct hadoop_err *err = NULL;
    char *host, *colon;
    uint32_t addr;
    int port;

    fprintf(stderr, "parse_rpc_addr(input=%s, default_port=%d)\n",
            input, default_port);

    // If the URI doesn't contain a port, we use a default.
    // This may come either from the hdfsBuilder, or from the
    // 'default default' for HDFS.
    // It's kind of silly that hdfsBuilder even includes this field, since this
    // information should just be included in the URI, but this is here for
    // compatibility.
    port = (default_port <= 0) ? DEFAULT_NN_PORT : default_port;
    host = strdup(input);
    if (!host) {
        err = hadoop_lerr_alloc(ENOMEM, "parse_rpc_addr: OOM");
        goto done;
    }
    colon = index(host, ':');
    if (colon) {
        // If the URI has a colon, we parse the next part as a port.
        char *port_str = colon + 1;
        *colon = '\0';
        port = atoi(colon);
        if ((port <= 0) || (port >= 65536)) {
            err = hadoop_lerr_alloc(EINVAL, "parse_rpc_addr: invalid port "
                                    "string %s", port_str);
            goto done;
        }
    }
    err = get_first_ipv4_addr(host, &addr);
    if (err)
        goto done;
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    out->sin_addr.s_addr = htonl(addr);
done:
    free(host);
    return err;
}

static struct hadoop_err *get_namenode_addr(const struct hadoop_uri *conn_uri,
        const struct hdfsBuilder *hdfs_bld, struct sockaddr_in *nn_addr)
{
    const char *nameservice_id;
    const char *rpc_addr;

    nameservice_id = hconf_get(hdfs_bld->hconf, "dfs.nameservice.id");
    if (nameservice_id) {
        return hadoop_lerr_alloc(ENOTSUP, "get_namenode_addr: we "
                "don't yet support HA or federated configurations.");
    }
    rpc_addr = hconf_get(hdfs_bld->hconf, "dfs.namenode.rpc-address");
    if (rpc_addr) {
        return parse_rpc_addr(rpc_addr, nn_addr, hdfs_bld->port);
    }
    return parse_rpc_addr(conn_uri->auth, nn_addr, hdfs_bld->port);
}

struct hadoop_err *ndfs_connect(struct hdfsBuilder *hdfs_bld,
                                struct hdfs_internal **out)
{
    struct hadoop_err *err = NULL;
    struct native_fs *fs = NULL;
    struct hrpc_messenger_builder *msgr_bld;
    struct ndfs_server_defaults defaults;
    int used_port;
    char *working_dir = NULL;

    fs = calloc(1, sizeof(*fs));
    if (!fs) {
        err = hadoop_lerr_alloc(ENOMEM, "failed to allocate space "
                                "for a native_fs structure.");
        goto done;
    }
    fs->base.ty = HADOOP_FS_TY_NDFS;
    fs->conn_uri = hdfs_bld->uri;
    hdfs_bld->uri = NULL;
    // Calculate our url_prefix.  We'll need this when spitting out URIs from
    // listStatus and getFileInfo.  We don't include the port in this URL
    // prefix unless it is non-standard.
    used_port = ntohs(fs->nn_addr.sin_port);
    if (used_port == DEFAULT_NN_PORT) {
        err = dynprintf(&fs->url_prefix, "%s://%s",
                     fs->conn_uri->scheme, fs->conn_uri->auth);
        if (err)
            goto done;
    } else {
        err = dynprintf(&fs->url_prefix, "%s://%s:%d",
                     fs->conn_uri->scheme, fs->conn_uri->auth,
                     used_port);
        if (err)
            goto done;
    }
    msgr_bld = hrpc_messenger_builder_alloc();
    if (!msgr_bld) {
        err = hadoop_lerr_alloc(ENOMEM, "failed to allocate space "
                                "for a messenger builder.");
        goto done;
    }
    err = get_namenode_addr(fs->conn_uri, hdfs_bld, &fs->nn_addr);
    if (err)
        goto done;
    err = hrpc_messenger_create(msgr_bld, &fs->msgr);
    if (err)
        goto done;
    // Get the default working directory
    if (uv_mutex_init(&fs->working_uri_lock) < 0) {
        err = hadoop_lerr_alloc(ENOMEM, "failed to create a mutex.");
        goto done;
    }
    err = dynprintf(&working_dir, "%s:///user/%s/",
                 fs->conn_uri->scheme, fs->conn_uri->user_info);
    if (err) {
        uv_mutex_destroy(&fs->working_uri_lock);
        goto done;
    }
    err = hadoop_uri_parse(working_dir, NULL, &fs->working_uri,
                           H_URI_PARSE_ALL | H_URI_APPEND_SLASH);
    if (err) {
        uv_mutex_destroy(&fs->working_uri_lock);
        err = hadoop_err_prepend(err, 0, "ndfs_connect: error parsing "
                                "working directory");
        goto done;
    }
    err = ndfs_connect_setup_conf(fs, hdfs_bld->hconf);
    if (err)
        goto done;

    // Ask the NameNode about our server defaults.  We'll use this information
    // later in ndfs_get_default_block_size, and when writing new files.  Just
    // as important, tghis validates that we can talk to the NameNode with our
    // current configuration.
    memset(&defaults, 0, sizeof(defaults));
    err = ndfs_get_server_defaults(fs, &defaults);
    if (err)
        goto done;
    fs->default_block_size = defaults.blocksize;
    err = NULL;

done:
    free(working_dir);
    if (err) {
        ndfs_free(fs);
        return err; 
    }
    *out = (struct hdfs_internal *)fs;
    return NULL; 
}

/**
 * Configure the native file system using the Hadoop configuration.
 *
 * @param fs            The filesystem to set configuration keys for.
 * @param hconf         The configuration object to read from.
 */
static struct hadoop_err *ndfs_connect_setup_conf(struct native_fs *fs,
                                                  struct hconf *hconf)
{
    struct hadoop_err *err = NULL;
    const char *umask_str;
    int64_t timeout_ms;

    umask_str = hconf_get(hconf, FS_PERMISSIONS_UMASK_KEY);
    if (!umask_str)
        umask_str = FS_PERMISSIONS_UMASK_DEFAULT;
    err = parse_permission(umask_str, &fs->umask);
    if (err) {
        return hadoop_err_prepend(err, 0, "ndfs_connect_setup_conf: "
                "error handling %s", FS_PERMISSIONS_UMASK_DEFAULT);
    }

    timeout_ms = DFS_CLIENT_WRITE_EXCLUDE_NODES_CACHE_EXPIRY_INTERVAL_DEFAULT;
    hconf_get_int64(hconf,
            DFS_CLIENT_WRITE_EXCLUDE_NODES_CACHE_EXPIRY_INTERVAL, &timeout_ms);
    fs->dead_dn_timeout_ns = timeout_ms * 1000LL;
    return NULL;
}

static void ndfs_free(struct native_fs *fs)
{
    if (fs->msgr) {
        hrpc_messenger_shutdown(fs->msgr);
        hrpc_messenger_free(fs->msgr);
    }
    free(fs->url_prefix);
    hadoop_uri_free(fs->conn_uri);
    if (fs->working_uri) {
        hadoop_uri_free(fs->working_uri);
        uv_mutex_destroy(&fs->working_uri_lock);
    }
    free(fs);
}

int ndfs_disconnect(hdfsFS bfs)
{
    struct native_fs *fs = (struct native_fs*)bfs;

    ndfs_free(fs);
    return 0;
}

int ndfs_file_exists(hdfsFS bfs, const char *uri)
{
    static hdfsFileInfo *info;

    info = ndfs_get_path_info(bfs, uri);
    if (!info) {
        // errno will be set
        return -1;
    }
    hdfsFreeFileInfo(info, 1);
    return 0;
}

int ndfs_unlink(struct hdfs_internal *bfs,
                const char *uri, int recursive)
{
    struct native_fs *fs = (struct native_fs*)bfs;
    struct hadoop_err *err = NULL;
    DeleteRequestProto req = DELETE_REQUEST_PROTO__INIT;
    struct hrpc_proxy proxy;
    DeleteResponseProto *resp = NULL;
    char *path = NULL;

    ndfs_nn_proxy_init(fs, &proxy);
    err = build_path(fs, uri, &path);
    if (err) {
        goto done;
    }
    req.src = path;
    req.recursive = !!recursive;
    err = cnn_delete(&proxy, &req, &resp);
    if (err) {
        goto done;
    }
    if (resp->result == 0) {
        err = hadoop_lerr_alloc(ENOENT, "ndfs_unlink(%s, recursive=%d): "
                    "deletion failed on the server", uri, recursive);
        goto done;
    }

done:
    free(path);
    if (resp) {
        delete_response_proto__free_unpacked(resp, NULL);
    }
    return hadoopfs_errno_and_retcode(err);
}

int ndfs_rename(hdfsFS bfs, const char *src_uri, const char *dst_uri)
{
    struct native_fs *fs = (struct native_fs*)bfs;
    struct hadoop_err *err = NULL;
    Rename2RequestProto req = RENAME2_REQUEST_PROTO__INIT;
    Rename2ResponseProto *resp = NULL;
    struct hrpc_proxy proxy;
    char *src_path = NULL, *dst_path = NULL;

    ndfs_nn_proxy_init(fs, &proxy);
    err = build_path(fs, src_uri, &src_path);
    if (err) {
        goto done;
    }
    err = build_path(fs, dst_uri, &dst_path);
    if (err) {
        goto done;
    }
    req.src = src_path;
    req.dst = dst_path;
    req.overwritedest = 0; // TODO: support overwrite
    err = cnn_rename2(&proxy, &req, &resp);
    if (err) {
        goto done;
    }

done:
    free(src_path);
    free(dst_path);
    if (resp) {
        rename2_response_proto__free_unpacked(resp, NULL);
    }
    return hadoopfs_errno_and_retcode(err);
}

char* ndfs_get_working_directory(hdfsFS bfs, char *buffer, size_t bufferSize)
{
    size_t len;
    struct native_fs *fs = (struct native_fs *)bfs;
    struct hadoop_err *err = NULL;

    uv_mutex_lock(&fs->working_uri_lock);
    len = strlen(fs->conn_uri->path);
    if (len + 1 > bufferSize) {
        err = hadoop_lerr_alloc(ENAMETOOLONG, "ndfs_get_working_directory: "
                "the buffer supplied was only %zd bytes, but we would need "
                "%zd bytes to hold the working directory.",
                bufferSize, len + 1);
        goto done;
    }
    strcpy(buffer, fs->conn_uri->path);
done:
    uv_mutex_unlock(&fs->working_uri_lock);
    return hadoopfs_errno_and_retptr(err, buffer);
}

int ndfs_set_working_directory(hdfsFS bfs, const char* uri_str)
{
    struct native_fs *fs = (struct native_fs *)bfs;
    struct hadoop_err *err = NULL;
    struct hadoop_uri *uri = NULL;

    uv_mutex_lock(&fs->working_uri_lock);
    err = hadoop_uri_parse(uri_str, fs->working_uri, &uri,
            H_URI_PARSE_ALL | H_URI_APPEND_SLASH);
    if (err) {
        err = hadoop_err_prepend(err, 0, "ndfs_set_working_directory: ");
        goto done;
    }
    hadoop_uri_free(fs->working_uri);
    fs->working_uri = uri;
    err = NULL;

done:
    uv_mutex_unlock(&fs->working_uri_lock);
    return hadoopfs_errno_and_retcode(err);
}

int ndfs_mkdir(hdfsFS bfs, const char* uri)
{
    struct native_fs *fs = (struct native_fs *)bfs;
    struct hadoop_err *err = NULL;
    MkdirsRequestProto req = MKDIRS_REQUEST_PROTO__INIT;
    MkdirsResponseProto *resp = NULL;
    FsPermissionProto perm = FS_PERMISSION_PROTO__INIT;
    struct hrpc_proxy proxy;
    char *path = NULL;

    ndfs_nn_proxy_init(fs, &proxy);
    err = build_path(fs, uri, &path);
    if (err) {
        goto done;
    }
    req.src = path;
    // TODO: a better libhdfs API would allow us to specify what mode to
    // create a particular directory with.
    perm.perm = 0777 & (~fs->umask);
    req.masked = &perm;
    req.createparent = 1; // TODO: add libhdfs API for non-recursive mkdir
    err = cnn_mkdirs(&proxy, &req, &resp);
    if (err) {
        goto done;
    }
    if (!resp->result) {
        err = hadoop_lerr_alloc(EEXIST, "ndfs_mkdir(%s): a path "
                "component already exists as a non-directory.", path);
        goto done;
    }
    err = NULL;

done:
    free(path);
    if (resp) {
        mkdirs_response_proto__free_unpacked(resp, NULL);
    }
    return hadoopfs_errno_and_retcode(err);
}

int ndfs_set_replication(hdfsFS bfs, const char* uri, int16_t replication)
{
    struct native_fs *fs = (struct native_fs *)bfs;
    struct hadoop_err *err = NULL;
    SetReplicationRequestProto req = SET_REPLICATION_REQUEST_PROTO__INIT;
    SetReplicationResponseProto *resp = NULL;
    struct hrpc_proxy proxy;
    char *path = NULL;

    ndfs_nn_proxy_init(fs, &proxy);
    err = build_path(fs, uri, &path);
    if (err) {
        goto done;
    }
    req.src = path;
    req.replication = replication;
    err = cnn_set_replication(&proxy, &req, &resp);
    if (err) {
        goto done;
    }
    if (!resp->result) {
        err = hadoop_lerr_alloc(EINVAL, "ndfs_set_replication(%s): path "
                "does not exist or is not a regular file.", path);
        goto done;
    }

done:
    free(path);
    if (resp) {
        set_replication_response_proto__free_unpacked(resp, NULL);
    }
    return hadoopfs_errno_and_retcode(err);
}

static struct hadoop_err *ndfs_list_partial(struct native_fs *fs,
        const char *path, const char *prev, uint32_t *entries_len,
        hdfsFileInfo **entries, uint32_t *remaining)
{
    struct hadoop_err *err = NULL;
    GetListingRequestProto req = GET_LISTING_REQUEST_PROTO__INIT;
    GetListingResponseProto *resp = NULL;
    hdfsFileInfo *nentries;
    struct hrpc_proxy proxy;
    uint64_t nlen;
    size_t i;
    char *prefix = NULL;

    err = dynprintf(&prefix, "%s%s/", fs->url_prefix, path);
    if (err)
        goto done;
    ndfs_nn_proxy_init(fs, &proxy);
    req.src = (char*)path;
    req.startafter.data = (unsigned char*)prev;
    req.startafter.len = strlen(prev);
    req.needlocation = 0;
    err = cnn_get_listing(&proxy, &req, &resp);
    if (err)
        goto done;
    if (!resp->dirlist) {
        err = hadoop_lerr_alloc(ENOENT, "ndfs_list_partial(path=%s, "
                "prev=%s): No such directory.", path, prev);
        goto done;
    }
    nlen = *entries_len;
    nlen += resp->dirlist->n_partiallisting;
    nentries = realloc(*entries, nlen * sizeof(hdfsFileInfo));
    if (!nentries) {
        err = hadoop_lerr_alloc(ENOENT, "ndfs_list_partial(path=%s, "
                "prev=%s): failed to allocate space for %zd new entries.",
                path, prev, resp->dirlist->n_partiallisting);
        goto done;
    }
    memset(nentries + ((*entries_len) * sizeof(hdfsFileInfo)),
           0, (resp->dirlist->n_partiallisting * sizeof(hdfsFileInfo)));
    *entries = nentries;
    *entries_len = nlen;
    *remaining = resp->dirlist->remainingentries;
    for (i = 0; i < resp->dirlist->n_partiallisting; i++) {
        err = populate_file_info(&nentries[i],
                    resp->dirlist->partiallisting[i], prefix);
        if (err)
            goto done;
    }
    err = NULL;

done:
    free(prefix);
    if (resp) {
        get_listing_response_proto__free_unpacked(resp, NULL);
    }
    return err;
}

static struct hadoop_err *populate_file_info(struct file_info *info,
                    HdfsFileStatusProto *status, const char *prefix)
{
    if (status->filetype == IS_DIR) {
        info->mKind = kObjectKindDirectory; 
    } else {
        // note: we don't support symlinks yet here.
        info->mKind = kObjectKindFile; 
    }
    info->mName = malloc(strlen(prefix) + status->path.len + 1);
    if (!info->mName)
        goto oom;
    strcpy(info->mName, prefix);
    memcpy(info->mName + strlen(prefix), status->path.data, status->path.len);
    info->mName[strlen(prefix) + status->path.len] = '\0';
    info->mLastMod = status->modification_time / 1000LL;
    info->mSize = status->length;
    if (status->has_block_replication) {
        info->mReplication = status->block_replication;
    } else {
        info->mReplication = 0;
    }
    if (status->has_blocksize) {
        info->mBlockSize = status->blocksize;
    } else {
        info->mBlockSize = 0;
    }
    info->mOwner = strdup(status->owner);
    if (!info->mOwner)
        goto oom;
    info->mGroup = strdup(status->group);
    if (!info->mGroup)
        goto oom;
    info->mPermissions = status->permission->perm;
    info->mLastAccess = status->access_time / 1000LL;
    return NULL;
oom:
    return hadoop_lerr_alloc(ENOMEM, "populate_file_info(%s): OOM",
                            info->mName);
}

hdfsFileInfo* ndfs_list_directory(hdfsFS bfs, const char* uri, int *numEntries)
{
    struct native_fs *fs = (struct native_fs *)bfs;
    struct hadoop_err *err = NULL;
    hdfsFileInfo *entries = NULL;
    uint32_t entries_len = 0, remaining = 0;
    char *prev, *path = NULL;

    err = build_path(fs, uri, &path);
    if (err) {
        goto done;
    }
    // We may need to make multiple RPCs to the Namenode to get all the
    // entries in this directory.  We need to keep making RPCs as long as the
    // 'remaining' value we get back is more than 0.  The actual value of
    // 'remaining' isn't interesting, because it may have changed by the time
    // we make the next RPC.
    do {
        if (entries_len > 0) {
            prev = entries[entries_len - 1].mName;
        } else {
            prev = "";
        }
        err = ndfs_list_partial(fs, path, prev, &entries_len,
                                &entries, &remaining);
        if (err)
            goto done;
    } while (remaining != 0);
    err = NULL;

done:
    free(path);
    if (err) {
        if (entries) {
            hdfsFreeFileInfo(entries, entries_len);
            entries = NULL;
        }
    } else {
        *numEntries = entries_len;
    }
    return hadoopfs_errno_and_retptr(err, entries);
}

hdfsFileInfo *ndfs_get_path_info(hdfsFS bfs, const char* uri)
{
    struct native_fs *fs = (struct native_fs*)bfs;
    struct hadoop_err *err = NULL;
    GetFileInfoRequestProto req = GET_FILE_INFO_REQUEST_PROTO__INIT;
    GetFileInfoResponseProto *resp = NULL;
    struct hrpc_proxy proxy;
    char *prefix = NULL, *path = NULL;
    hdfsFileInfo *info = NULL;

    // The GetFileInfo RPC returns a blank 'path' field.
    // To maintain 100% compatibility with the JNI client, we need to fill it
    // in with a URI containing the absolute path to the file.
    err = dynprintf(&prefix, "%s%s", fs->url_prefix, path);
    if (err)
        goto done;
    ndfs_nn_proxy_init(fs, &proxy);
    err = build_path(fs, uri, &path);
    if (err) {
        goto done;
    }
    req.src = path;
    err = cnn_get_file_info(&proxy, &req, &resp);
    if (err) {
        goto done;
    }
    if (!resp->fs) {
        err = hadoop_lerr_alloc(ENOENT, "ndfs_get_path_info(%s): no such "
                                "file or directory.", path);
        goto done;
    }
    info = calloc(1, sizeof(*info));
    if (!info) {
        err = hadoop_lerr_alloc(ENOMEM, "ndfs_get_path_info(%s): OOM", path);
        goto done;
    }
    err = populate_file_info(info, resp->fs, prefix);
    if (err) {
        err = hadoop_err_prepend(err, 0, "ndfs_get_path_info(%s)", path);
        goto done;
    }
    err = NULL;

done:
    free(prefix);
    free(path);
    if (resp) {
        get_file_info_response_proto__free_unpacked(resp, NULL);
    }
    if (err) {
        if (info) {
            hdfsFreeFileInfo(info, 1);
            info = NULL;
        }
    }
    return hadoopfs_errno_and_retptr(err, info);
}

tOffset ndfs_get_default_block_size(hdfsFS bfs)
{
    struct native_fs *fs = (struct native_fs *)bfs;
    return fs->default_block_size;
}

tOffset ndfs_get_default_block_size_at_path(hdfsFS bfs, const char *uri)
{
    struct native_fs *fs = (struct native_fs *)bfs;
    struct hadoop_err *err = NULL;
    GetPreferredBlockSizeRequestProto req =
        GET_PREFERRED_BLOCK_SIZE_REQUEST_PROTO__INIT;
    GetPreferredBlockSizeResponseProto *resp = NULL;
    struct hrpc_proxy proxy;
    tOffset ret = 0;
    char *path = NULL;

    ndfs_nn_proxy_init(fs, &proxy);
    err = build_path(fs, uri, &path);
    if (err) {
        goto done;
    }
    req.filename = path;
    err = cnn_get_preferred_block_size(&proxy, &req, &resp);
    if (err) {
        goto done;
    }
    ret = resp->bsize;
    err = NULL;

done:
    free(path);
    if (resp) {
        get_preferred_block_size_response_proto__free_unpacked(resp, NULL);
    }
    if (err)
        return hadoopfs_errno_and_retcode(err);
    return ret;
}

struct hadoop_err *ndfs_statvfs(struct hadoop_fs_base *hfs,
        struct hadoop_vfs_stats *stats)
{
    struct native_fs *fs = (struct native_fs*)hfs;

    GetFsStatusRequestProto req = GET_FS_STATUS_REQUEST_PROTO__INIT;
    GetFsStatsResponseProto *resp = NULL;
    struct hadoop_err *err = NULL;
    struct hrpc_proxy proxy;

    ndfs_nn_proxy_init(fs, &proxy);
    err = cnn_get_fs_stats(&proxy, &req, &resp);
    if (err) {
        goto done;
    }
    stats->capacity = resp->capacity;
    stats->used = resp->used;
    stats->remaining = resp->remaining;
    stats->under_replicated = resp->under_replicated;
    stats->corrupt_blocks = resp->corrupt_blocks;
    stats->missing_blocks = resp->missing_blocks;

done:
    if (resp) {
        get_fs_stats_response_proto__free_unpacked(resp, NULL);
    }
    return err;
}

tOffset ndfs_get_capacity(hdfsFS bfs)
{
    struct hadoop_err *err;
    struct hadoop_vfs_stats stats;

    err = ndfs_statvfs((struct hadoop_fs_base *)bfs, &stats);
    if (err)
        return hadoopfs_errno_and_retcode(err);
    return stats.capacity;
}

tOffset ndfs_get_used(hdfsFS bfs)
{
    struct hadoop_err *err;
    struct hadoop_vfs_stats stats;

    err = ndfs_statvfs((struct hadoop_fs_base *)bfs, &stats);
    if (err)
        return hadoopfs_errno_and_retcode(err);
    return stats.used;
}

int ndfs_chown(hdfsFS bfs, const char* uri,
                      const char *user, const char *group)
{
    struct native_fs *fs = (struct native_fs *)bfs;
    struct hadoop_err *err = NULL;
    SetOwnerRequestProto req = SET_OWNER_REQUEST_PROTO__INIT;
    SetOwnerResponseProto *resp = NULL;
    struct hrpc_proxy proxy;
    char *path = NULL;

    ndfs_nn_proxy_init(fs, &proxy);
    err = build_path(fs, uri, &path);
    if (err) {
        goto done;
    }
    req.src = path;
    req.username = (char*)user;
    req.groupname = (char*)group;
    err = cnn_set_owner(&proxy, &req, &resp);
    if (err) {
        goto done;
    }

done:
    free(path);
    if (resp) {
        set_owner_response_proto__free_unpacked(resp, NULL);
    }
    return hadoopfs_errno_and_retcode(err);
}

int ndfs_chmod(hdfsFS bfs, const char* uri, short mode)
{
    struct native_fs *fs = (struct native_fs *)bfs;
    FsPermissionProto perm = FS_PERMISSION_PROTO__INIT;
    SetPermissionRequestProto req = SET_PERMISSION_REQUEST_PROTO__INIT;
    SetPermissionResponseProto *resp = NULL;
    struct hadoop_err *err = NULL;
    struct hrpc_proxy proxy;
    char *path = NULL;

    ndfs_nn_proxy_init(fs, &proxy);
    err = build_path(fs, uri, &path);
    if (err) {
        goto done;
    }
    req.src = path;
    req.permission = &perm;
    perm.perm = mode;
    err = cnn_set_permission(&proxy, &req, &resp);
    if (err) {
        goto done;
    }

done:
    free(path);
    if (resp) {
        set_permission_response_proto__free_unpacked(resp, NULL);
    }
    return hadoopfs_errno_and_retcode(err);
}

int ndfs_utime(hdfsFS bfs, const char* uri, int64_t mtime, int64_t atime)
{
    struct native_fs *fs = (struct native_fs *)bfs;
    SetTimesRequestProto req = SET_TIMES_REQUEST_PROTO__INIT ;
    SetTimesResponseProto *resp = NULL;
    struct hadoop_err *err = NULL;
    struct hrpc_proxy proxy;
    char *path = NULL;

    ndfs_nn_proxy_init(fs, &proxy);
    err = build_path(fs, uri, &path);
    if (err) {
        goto done;
    }
    req.src = path;
    // If mtime or atime are -1, that means "no change."
    // Otherwise, we need to multiply by 1000, to take into account the fact
    // that libhdfs times are in seconds, and HDFS times are in milliseconds.
    // It's unfortunate that libhdfs doesn't support the full millisecond
    // precision.  We need to redo the API at some point.
    if (mtime < 0) {
        req.mtime = -1;
    } else {
        req.mtime = mtime;
        req.mtime *= 1000;
    }
    if (atime < 0) {
        req.atime = -1;
    } else {
        req.atime = atime;
        req.atime *= 1000;
    }
    err = cnn_set_times(&proxy, &req, &resp);
    if (err) {
        goto done;
    }

done:
    free(path);
    if (resp) {
        set_times_response_proto__free_unpacked(resp, NULL);
    }
    return hadoopfs_errno_and_retcode(err);
}

// vim: ts=4:sw=4:tw=79:et
