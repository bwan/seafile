/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <string.h>
#include <ccnet.h>
#include <stdlib.h>

#include <ccnet.h>
#include <ccnet/job-mgr.h>
#include <ccnet/ccnet-object.h>

#include "common.h"

#define DEBUG_FLAG SEAFILE_DEBUG_TRANSFER
#include "log.h"

#include "seafile-session.h"
#include "utils.h"

#include "check-tx-slave-v2-proc.h"

#define SC_GET_TOKEN        "301"
#define SS_GET_TOKEN        "Get token"
#define SC_PUT_TOKEN        "302"
#define SS_PUT_TOKEN        "Put token"
#define SC_GET_VERSION      "303"
#define SS_GET_VERSION      "Get version"
#define SC_VERSION          "304"
#define SS_VERSION          "Version"

#define SC_ACCESS_DENIED    "401"
#define SS_ACCESS_DENIED    "Access denied"
#define SC_SERVER_ERROR     "404"
#define SS_SERVER_ERROR     "Internal server error"
#define SC_PROTOCOL_MISMATCH "405"
#define SS_PROTOCOL_MISMATCH "Protocol version mismatch"

/* Only for upload */
#define SC_QUOTA_ERROR      "402"
#define SS_QUOTA_ERROR      "Failed to get quota"
#define SC_QUOTA_FULL       "403"
#define SS_QUOTA_FULL       "storage for the repo's owner is full"

/* Only for download */
#define SC_BAD_REPO         "406"
#define SS_BAD_REPO         "Repo doesn't exist"
#define SC_NO_BRANCH        "407"
#define SS_NO_BRANCH        "Branch not found"

enum {
    INIT,
    ACCESS_GRANTED,
};

enum {
    CHECK_TX_TYPE_UPLOAD,
    CHECK_TX_TYPE_DOWNLOAD,
};

typedef struct {
    int type;

    char repo_id[37];
    char *branch_name;
    char *token;
    char *session_key;

    char *rsp_code;
    char *rsp_msg;
    char head_id[41];
    int has_branch;
} SeafileCheckTxSlaveV2ProcPriv;

#define GET_PRIV(o)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((o), SEAFILE_TYPE_CHECK_TX_SLAVE_V2_PROC, SeafileCheckTxSlaveV2ProcPriv))

#define USE_PRIV \
    SeafileCheckTxSlaveV2ProcPriv *priv = GET_PRIV(processor);

G_DEFINE_TYPE (SeafileCheckTxSlaveV2Proc, seafile_check_tx_slave_v2_proc, CCNET_TYPE_PROCESSOR)

static int start (CcnetProcessor *processor, int argc, char **argv);
static void handle_update (CcnetProcessor *processor,
                           char *code, char *code_msg,
                           char *content, int clen);
static void thread_done (void *result);

static void
release_resource(CcnetProcessor *processor)
{
    USE_PRIV;

    /* g_free works fine even if ptr is NULL. */
    g_free (priv->token);
    g_free (priv->session_key);
    g_free (priv->branch_name);
    g_free (priv->rsp_code);
    g_free (priv->rsp_msg);

    CCNET_PROCESSOR_CLASS (seafile_check_tx_slave_v2_proc_parent_class)->release_resource (processor);
}


static void
seafile_check_tx_slave_v2_proc_class_init (SeafileCheckTxSlaveV2ProcClass *klass)
{
    CcnetProcessorClass *proc_class = CCNET_PROCESSOR_CLASS (klass);

    proc_class->name = "check-tx-slave-v2-proc";
    proc_class->start = start;
    proc_class->handle_update = handle_update;
    proc_class->release_resource = release_resource;

    g_type_class_add_private (klass, sizeof (SeafileCheckTxSlaveV2ProcPriv));
}

static void
seafile_check_tx_slave_v2_proc_init (SeafileCheckTxSlaveV2Proc *processor)
{
}

static void
get_branch_head (CcnetProcessor *processor)
{
    SeafBranch *branch;
    USE_PRIV;

    branch = seaf_branch_manager_get_branch (seaf->branch_mgr, 
                                             priv->repo_id, priv->branch_name);
    if (branch != NULL) {
        priv->has_branch = 1;
        memcpy (priv->head_id, branch->commit_id, 41);
        seaf_branch_unref (branch);

        priv->rsp_code = g_strdup(SC_OK);
        priv->rsp_msg = g_strdup(SS_OK);
    } else if (priv->type == CHECK_TX_TYPE_UPLOAD) {
        priv->rsp_code = g_strdup(SC_OK);
        priv->rsp_msg = g_strdup(SS_OK);
    } else {
        priv->rsp_code = g_strdup(SC_NO_BRANCH);
        priv->rsp_msg = g_strdup(SS_NO_BRANCH);
    }
}

static int
decrypt_token (CcnetProcessor *processor)
{
    USE_PRIV;
    int hex_len, encrypted_len, token_len; 
    char *encrypted_token = NULL;
    SeafileCrypt *crypt = NULL;
    unsigned char key[16], iv[16];
    char *token = NULL;
    int ret = 0;

    /* raw data is half the length of hexidecimal */
    hex_len = strlen(priv->token);
    if (hex_len % 2 != 0) {
        seaf_warning ("[check tx slave v2] invalid length of encrypted token\n"); 
        ret = -1;
        goto out;
    }

    token = seaf_repo_manager_get_decrypted_token (seaf->repo_mgr,
                                                   priv->token,
                                                   priv->session_key);
    if (token)
        goto found;
    
    encrypted_len = hex_len / 2;
    encrypted_token = g_malloc (encrypted_len);
    hex_to_rawdata (priv->token,
                    (unsigned char *)encrypted_token,
                    encrypted_len);

    seafile_generate_enc_key (priv->session_key,
                              strlen(priv->session_key),
                              CURRENT_ENC_VERSION, key, iv);
    crypt = seafile_crypt_new (CURRENT_ENC_VERSION, key, iv);
    
    if (seafile_decrypt (&token, &token_len, encrypted_token,
                         encrypted_len, crypt) < 0) {
        seaf_warning ("[check tx slave v2] failed to decrypt token\n");
        ret = -1;
        goto out;
    }

    /* Add to cache. */
    seaf_repo_manager_add_decrypted_token (seaf->repo_mgr,
                                           priv->token,
                                           priv->session_key,
                                           token);

found:
    g_free (priv->token);
    /* we can use the decrypted data directly, since the trailing null byte is
     * also included when encrypting in the client */
    priv->token = token;

out:
    g_free (crypt);
    g_free (encrypted_token);
    
    return ret;
}

static void *
check_tx (void *vprocessor)
{
    CcnetProcessor *processor = vprocessor;
    USE_PRIV;

    char *user = NULL;
    char *repo_id = priv->repo_id;

    if (!seaf_repo_manager_repo_exists (seaf->repo_mgr, repo_id)) {
        priv->rsp_code = g_strdup(SC_BAD_REPO);
        priv->rsp_msg = g_strdup(SS_BAD_REPO);
        goto out;
    }

    if (decrypt_token (processor) < 0) {
        priv->rsp_code = g_strdup(SC_ACCESS_DENIED);
        priv->rsp_msg = g_strdup(SS_ACCESS_DENIED);
        goto out;
    }

    user = seaf_repo_manager_get_email_by_token (
        seaf->repo_mgr, repo_id, priv->token);
    
    if (!user) {
        priv->rsp_code = g_strdup(SC_ACCESS_DENIED);
        priv->rsp_msg = g_strdup(SS_ACCESS_DENIED);
        goto out;
    }

    if (priv->type == CHECK_TX_TYPE_UPLOAD &&
        seaf_quota_manager_check_quota (seaf->quota_mgr, repo_id) < 0) {
        priv->rsp_code = g_strdup(SC_QUOTA_FULL);
        priv->rsp_msg = g_strdup(SS_QUOTA_FULL);
        goto out;
    }
    
    char *perm = seaf_repo_manager_check_permission (seaf->repo_mgr,
                                                     repo_id, user, NULL);
    if (!perm ||
        (strcmp (perm, "r") == 0 && priv->type == CHECK_TX_TYPE_UPLOAD))
    {
        priv->rsp_code = g_strdup(SC_ACCESS_DENIED);
        priv->rsp_msg = g_strdup(SS_ACCESS_DENIED);
        g_free (perm);
        goto out;
    }
    g_free (perm);

    get_branch_head (processor);

out:
    g_free (user);
    return vprocessor;    
}

static void 
thread_done (void *result)
{
    CcnetProcessor *processor = result;

    if (processor->delay_shutdown) {
        ccnet_processor_done (processor, FALSE);
        return;
    }

    USE_PRIV;

    if (strcmp (priv->rsp_code, SC_OK) == 0) {
        if (priv->has_branch) {
            ccnet_processor_send_response (processor, 
                                           SC_OK, SS_OK, 
                                           priv->head_id, 41);
        } else
            ccnet_processor_send_response (processor, SC_OK, SS_OK, NULL, 0);
        processor->state = ACCESS_GRANTED;
    } else {
        ccnet_processor_send_response (processor,
                                       priv->rsp_code, priv->rsp_msg,
                                       NULL, 0);
        ccnet_processor_done (processor, FALSE);
    }
}

static int
start (CcnetProcessor *processor, int argc, char **argv)
{
    char *repo_id, *branch_name, *token;
    USE_PRIV;

    if (argc != 5) {
        ccnet_processor_send_response (processor, SC_BAD_ARGS, SS_BAD_ARGS, NULL, 0);
        ccnet_processor_done (processor, FALSE);
        return -1;
    }

    if (strcmp (argv[0], "upload") == 0) {
        priv->type = CHECK_TX_TYPE_UPLOAD;
    } else if (strcmp (argv[0], "download") == 0) {
        priv->type = CHECK_TX_TYPE_DOWNLOAD;
    } else {
        ccnet_processor_send_response (processor, SC_BAD_ARGS, SS_BAD_ARGS, NULL, 0);
        ccnet_processor_done (processor, FALSE);
        return -1;
    }

    repo_id = argv[2];
    branch_name = argv[3];
    token = argv[4];

    if (strlen(repo_id) != 36) {
        ccnet_processor_send_response (processor, SC_BAD_ARGS, SS_BAD_ARGS, NULL, 0);
        ccnet_processor_done (processor, FALSE);
        return -1;
    }

    if (priv->type == CHECK_TX_TYPE_UPLOAD &&
        strcmp (branch_name, "master") != 0) {
        ccnet_processor_send_response (processor, SC_BAD_ARGS, SS_BAD_ARGS, NULL, 0);
        ccnet_processor_done (processor, FALSE);
        return -1;
    }

    memcpy (priv->repo_id, repo_id, 37);
    priv->branch_name = g_strdup(branch_name);

    priv->token = g_strdup(token);

    CcnetPeer *peer = ccnet_get_peer (seaf->ccnetrpc_client, processor->peer_id);
    if (!peer || !peer->session_key) {
        seaf_warning ("[check tx slave v2] session key of peer %.10s is null\n",
                      processor->peer_id);
        ccnet_processor_send_response (processor, SC_BAD_PEER, SS_BAD_PEER, NULL, 0);
        ccnet_processor_done (processor, FALSE);
        if (peer)
            g_object_unref (peer);
        return -1;
    }

    priv->session_key = g_strdup(peer->session_key);
    g_object_unref (peer);

    seaf_debug ("[check-tx] %s repo %.8s.\n", argv[0], repo_id);

    ccnet_processor_thread_create (processor, seaf->job_mgr,
                                   check_tx, thread_done, processor);

    return 0;
}

static void
handle_update (CcnetProcessor *processor,
               char *code, char *code_msg,
               char *content, int clen)
{
    USE_PRIV;
    char *token;

    if (processor->state != ACCESS_GRANTED) {
        ccnet_processor_send_response (processor,
                                       SC_BAD_UPDATE_CODE, SS_BAD_UPDATE_CODE,
                                       NULL, 0);
        ccnet_processor_done (processor, FALSE);
        return;
    }

    if (strncmp (code, SC_GET_TOKEN, 3) == 0) {
        token = seaf_token_manager_generate_token (seaf->token_mgr,
                                                   processor->peer_id,
                                                   priv->repo_id);
        ccnet_processor_send_response (processor,
                                       SC_PUT_TOKEN, SS_PUT_TOKEN,
                                       token, strlen(token) + 1);
        g_free (token);
        return;
    } else if (strncmp (code, SC_GET_VERSION, 3) == 0) {
        char buf[16];
        int len;
        len = snprintf (buf, sizeof(buf), "%d", CURRENT_PROTO_VERSION);
        ccnet_processor_send_response (processor,
                                       SC_VERSION, SS_VERSION,
                                       buf, len + 1);
        ccnet_processor_done (processor, TRUE);
        return;
    }

    ccnet_processor_send_response (processor,
                                   SC_BAD_UPDATE_CODE, SS_BAD_UPDATE_CODE,
                                   NULL, 0);
    ccnet_processor_done (processor, FALSE);
}
