// Copyright (c) 2014 Hugo Peixoto, hugopeixoto.net

#include "fish2.h"
#include "key_store.h"
#include "settings.h"

#include "fish2/blowcrypt.h"
#include "fish2/noop.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define KEYBUF_SIZE 150
#define CONTACT_SIZE 100

struct fish2_s {
  key_store_t key_store;
  settings_t settings;
  char* prefix;
  char* filepath;
};

int fish2_init (
    fish2_t* ctx,
    const char* filepath)
{
    (*ctx) = (fish2_t)malloc(sizeof(struct fish2_s));
    if (key_store_init(&(*ctx)->key_store, filepath) < 0) {
        free(*ctx);
        return -1;
    }

    (*ctx)->prefix = strdup("+OK ");
    (*ctx)->filepath = strdup(filepath);

    return 0;
}

int fish2_rekey (fish2_t ctx, const char* new_key)
{
    return key_store_recrypt(ctx->key_store, new_key);
}

int fish2_has_master_key (fish2_t ctx)
{
    return key_store_has_master_key(ctx->key_store);
}

int fish2_validate_master_key (fish2_t ctx, const char* key)
{
    return key_store_validate_master_key(ctx->key_store, key);
}

void fish2_deinit (fish2_t ctx)
{
    key_store_deinit(ctx->key_store);
    free(ctx->prefix);
    free(ctx->filepath);
    free(ctx);
}

static int fish2_get_contact (
    fish2_t ctx,
    const char* server_tag,
    const char* target,
    char* contact)
{
    memset(contact, 0, CONTACT_SIZE);

    if (server_tag == NULL) {
        snprintf(contact, CONTACT_SIZE, "%s", target);
    } else {
        snprintf(contact, CONTACT_SIZE, "%s:%s", server_tag, target);
    }

    // Should INI fixing be here?
    return 0;
}

int fish2_has_key (
  fish2_t ctx,
  const char* server_tag,
  const char* receiver)
{
    return fish2_get_key(ctx, server_tag, receiver, NULL) == 0;
}

int fish2_get_key (
    fish2_t ctx,
    const char* server_tag,
    const char* receiver,
    char* key)
{
    char contact[CONTACT_SIZE] = { '\0' };

    if (fish2_get_contact(ctx, server_tag, receiver, contact) < 0) {
        return -1;
    }

    return key_store_get(ctx->key_store, contact, key);
}

int fish2_set_key (
    fish2_t ctx,
    const char* server_tag,
    const char* receiver,
    const char* key)
{
    char contact[CONTACT_SIZE] = { '\0' };

    if (fish2_get_contact(ctx, server_tag, receiver, contact) < 0) {
        return -1;
    }

    return key_store_set(ctx->key_store, contact, key);
}

int fish2_unset_key (
    fish2_t ctx,
    const char* server_tag,
    const char* receiver)
{
    char contact[CONTACT_SIZE] = { '\0' };

    if (fish2_get_contact(ctx, server_tag, receiver, contact) < 0) {
        return -1;
    }

    return key_store_unset(ctx->key_store, contact);
}

typedef int (*decrypter_f)(
    const char*, const char*,
    size_t, char**, size_t*, int*);

typedef int (*encrypter_f)(
    const char*, const char*,
    size_t, char**, size_t*);

struct decrypter_s {
    decrypter_f func;
    size_t offset;
};

struct encrypter_s {
    encrypter_f func;
    size_t offset;
};

static int fish2_get_decrypter (
    fish2_t ctx,
    const char* encrypted,
    size_t n,
    struct decrypter_s* decrypter)
{
    if (strncmp(encrypted, "+OK ", 4) == 0) {
        decrypter->func   = &fish2_blowfish_decrypt;
        decrypter->offset = 4;
        return 0;
    }

    if (strncmp(encrypted, "mcps ", 5) == 0) {
        decrypter->func   = &fish2_blowfish_decrypt;
        decrypter->offset = 5;
        return 0;
    }

    return -1;
}

static int fish2_get_encrypter (
    fish2_t ctx,
    const char* unencrypted,
    size_t n,
    struct encrypter_s* encrypter)
{
    // TODO(hpeixoto): settings_get_string("plain_prefix")
    if (strncmp(unencrypted, "+p ", strlen("+p ")) == 0) {
        encrypter->func   = &fish2_noop_encrypt;
        encrypter->offset = 4;
    } else {
        encrypter->func   = &fish2_blowfish_encrypt;
        encrypter->offset = 0;
    }

    return 0;
}

int fish2_encrypt (
    fish2_t ctx,
    const char* server_tag,
    const char* receiver,
    const char* plaintext,
    char* encrypted,
    size_t n)
{
    char key[KEYBUF_SIZE] = { '\0' };
    size_t input_size = strlen(plaintext);
    char* ciphertext = NULL;
    size_t ciphersize = 0;

    struct encrypter_s encrypter;
    if (fish2_get_encrypter(
            ctx,
            plaintext,
            input_size,
            &encrypter) < 0) {
        return -3;
    }

    if (fish2_get_key(ctx, server_tag, receiver, key) < 0) {
        return -1;
    }

    if (encrypter.func(
            key,
            plaintext + encrypter.offset,
            input_size - encrypter.offset,
            &ciphertext,
            &ciphersize) < 0) {
        return -2;
    }

    snprintf(encrypted, n, "%s%s", ctx->prefix, ciphertext);

    memset(key, 0, KEYBUF_SIZE);
    memset(ciphertext, 0, ciphersize);
    free(ciphertext);

    return 0;
}

int fish2_mark_encryption (
    fish2_t ctx,
    const char* server_tag,
    const char* sender,
    const char* plaintext,
    int broken,
    char* marked,
    size_t n)
{
    struct encrypter_s encrypter;
    char encryption_mark[32]   = { '\0' };
    char broken_block_mark[32] = { '\0' };

    if (fish2_get_encrypter(
            ctx,
            plaintext,
            strlen(plaintext),
            &encrypter) < 0) {
        return -1;
    }

    settings_get_string(
        ctx->settings,
        FISH2_SETTINGS_ENCRYPTION_MARK,
        encryption_mark,
        sizeof(encryption_mark));

    if (broken) {
      settings_get_string(
          ctx->settings,
          FISH2_SETTINGS_BROKEN_BLOCK_MARK,
          broken_block_mark,
          sizeof(broken_block_mark));
    }

    // TODO(hpeixoto): mark could be at the end: settings_get_int("mark_position")
    snprintf(
        marked, n,
        "%s%s%s",
        encryption_mark,
        plaintext + encrypter.offset,
        broken_block_mark);

    return 0;
}

int fish2_decrypt (
    fish2_t ctx,
    const char* server_tag,
    const char* sender,
    const char* encrypted,
    char* unencrypted,
    size_t n)
{
    char key[KEYBUF_SIZE] = { '\0' };
    size_t input_size = strlen(encrypted);
    char* plaintext = NULL;
    size_t plainsize = 0;
    int broken = 0;

    struct decrypter_s decrypter;
    if (fish2_get_decrypter(
            ctx,
            encrypted,
            input_size,
            &decrypter) < 0) {
        return -3;
    }

    if (fish2_get_key(ctx, server_tag, sender, key) < 0) {
        return -1;
    }

    if (decrypter.func(
            key,
            encrypted + decrypter.offset,
            input_size - decrypter.offset,
            &plaintext,
            &plainsize,
            &broken) < 0) {
        return -2;
    }

    fish2_mark_encryption(
        ctx,
        server_tag,
        sender,
        plaintext,
        broken,
        unencrypted,
        n);

    memset(key, 0, KEYBUF_SIZE);
    memset(plaintext, 0, plainsize);
    free(plaintext);

    return 0;
}
