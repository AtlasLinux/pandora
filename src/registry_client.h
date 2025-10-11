#ifndef REGISTRY_CLIENT_H
#define REGISTRY_CLIENT_H

#include <stddef.h>
#include "acl.h"

/* Opaque registry client handle */
typedef struct RegistryClient RegistryClient;

/* Create/destroy client (calls curl_global_init internally once) */
RegistryClient *registry_client_create(void);
void registry_client_destroy(RegistryClient *c);

/* Configure a registry index URL (simple single-registry API for now) */
int registry_client_set_index(RegistryClient *c, const char *index_url);

/* Fetch index.acl into an AclBlock (caller owns result) */
AclBlock *registry_client_fetch_index(RegistryClient *c);

/* Given a manifest URL, fetch and parse manifest.acl returning AclBlock* (caller owns) */
AclBlock *registry_client_fetch_manifest(RegistryClient *c, const char *manifest_url);

/* Helper: find PKG_URL_<version> or MANIFEST_URL_<version> in index by reading ACL */
char *registry_client_find_pkg_url(AclBlock *index, const char *pkg_name, const char *version);
char *registry_client_find_manifest_url(AclBlock *index, const char *pkg_name, const char *version);

#endif /* REGISTRY_CLIENT_H */
