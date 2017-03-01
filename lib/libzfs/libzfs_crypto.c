/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2017, Datto, Inc. All rights reserved.
 */

#include <sys/zfs_context.h>
#include <sys/fs/zfs.h>
#include <sys/dsl_crypt.h>
#include <sys/crypto/icp.h>
#include <libintl.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <libzfs.h>
#include "libzfs_impl.h"
#include "zfeature_common.h"

/*
 * User keys are used to decrypt the master encryption keys of a dataset. This
 * indirection allows a user to change his / her access key without having to
 * re-encrypt the entire dataset. User keys can be provided in one of several
 * ways. Raw keys are simlply given to the kernel as is. Similarly, hex keys
 * are converted to binary and passed into the kernel. Password based keys are
 * a bit more complicated. Passwords alone do not provide suitable entropy for
 * encryption and may be too short or too long to be used. In order to derive
 * a more appropriate key we use a PBKDF2 function. This function is designed
 * to take a (relatively) long time to calculate in order to discourage
 * attackers from guessing from a list of common passwords. PBKDF2 requires
 * 2 additional parameters. The first is the number of iterations to run, which
 * will ultimately decide how long it takes to derive the resulting key from
 * the password. The second parameter is a salt that is randomly generated for
 * each datasset. The salt is used to "tweak" PBKDF2 such that a group of
 * attackers cannot reasonably generate a table of commonly known passwords to
 * their output keys and expect it work for all past and future PBKDF2 users.
 * We store the salt as a hidden property of the dataset (although it is
 * technically ok if the salt is known to the attacker).
 */

typedef enum key_locator {
	KEY_LOCATOR_NONE,
	KEY_LOCATOR_PROMPT,
	KEY_LOCATOR_URI
} key_locator_t;

#define	MIN_PASSPHRASE_LEN 8
#define	MAX_PASSPHRASE_LEN 64
#define	MAX_KEY_PROMPT_ATTEMPTS 3

static int caught_interrupt;

static zfs_keylocation_t
zfs_prop_parse_keylocation(const char *str)
{
	if (strcmp("prompt", str) == 0)
		return (ZFS_KEYLOCATION_PROMPT);
	else if (strlen(str) > 8 && strncmp("file:///", str, 8) == 0)
		return (ZFS_KEYLOCATION_URI);

	return (ZFS_KEYLOCATION_NONE);
}

static int
hex_key_to_raw(char *hex, int hexlen, uint8_t *out)
{
	int ret, i;
	unsigned int c;

	for (i = 0; i < hexlen; i += 2) {
		if (!isxdigit(hex[i]) || !isxdigit(hex[i + 1])) {
			ret = EINVAL;
			goto error;
		}

		ret = sscanf(&hex[i], "%02x", &c);
		if (ret != 1) {
			ret = EINVAL;
			goto error;
		}

		out[i / 2] = c;
	}

	return (0);

error:
	return (ret);
}


static void
catch_signal(int sig)
{
	caught_interrupt = sig;
}

static char *
get_format_prompt_string(zfs_keyformat_t format)
{
	switch (format) {
	case ZFS_KEYFORMAT_RAW:
		return ("raw key");
	case ZFS_KEYFORMAT_HEX:
		return ("hex key");
	case ZFS_KEYFORMAT_PASSPHRASE:
		return ("passphrase");
	default:
		/* shouldn't happen */
		return (NULL);
	}
}

static int
get_key_material_raw(FILE *fd, const char *fsname, zfs_keyformat_t keyformat,
    boolean_t again, boolean_t newkey, uint8_t **buf, size_t *len_out)
{
	int ret = 0, bytes;
	size_t buflen = 0;
	struct termios old_term, new_term;
	struct sigaction act, osigint, osigtstp;

	*len_out = 0;

	if (isatty(fileno(fd))) {
		/*
		 * handle SIGINT and ignore SIGSTP. This is necessary to
		 * restore the state of the terminal.
		 */
		caught_interrupt = 0;
		act.sa_flags = 0;
		(void) sigemptyset(&act.sa_mask);
		act.sa_handler = catch_signal;

		(void) sigaction(SIGINT, &act, &osigint);
		act.sa_handler = SIG_IGN;
		(void) sigaction(SIGTSTP, &act, &osigtstp);

		/* prompt for the key */
		if (fsname != NULL) {
			(void) printf("%s %s%s for '%s': ",
			    (again) ? "Re-enter" : "Enter",
			    (newkey) ? "new " : "",
			    get_format_prompt_string(keyformat), fsname);
		} else {
			(void) printf("%s %s%s: ",
			    (again) ? "Re-enter" : "Enter",
			    (newkey) ? "new " : "",
			    get_format_prompt_string(keyformat));

		}
		(void) fflush(stdout);

		/* disable the terminal echo for key input */
		(void) tcgetattr(fileno(fd), &old_term);

		new_term = old_term;
		new_term.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);

		ret = tcsetattr(fileno(fd), TCSAFLUSH, &new_term);
		if (ret != 0) {
			ret = errno;
			errno = 0;
			goto out;
		}
	}

	/* read the key material */
	if (keyformat != ZFS_KEYFORMAT_RAW) {
		bytes = getline((char **)buf, &buflen, fd);
		if (bytes < 0) {
			ret = errno;
			errno = 0;
			goto out;
		}
	} else {
		/*
		 * Raw keys may have newline characters in them and so can't
		 * use getline(). Here we attempt to read 33 bytes so that we
		 * can properly check the key length (the file should only have
		 * 32 bytes).
		 */
		*buf = malloc((WRAPPING_KEY_LEN + 1) * sizeof (char));
		if (*buf == NULL) {
			ret = ENOMEM;
			goto out;
		}

		bytes = fread(*buf, 1, WRAPPING_KEY_LEN + 1, fd);
		if (bytes < 0) {
			/* size errors are handled by the calling function */
			free(*buf);
			*buf = NULL;
			ret = errno;
			errno = 0;
			goto out;
		}
	}

	/* trim the ending newline if it exists */
	if ((*buf)[bytes - 1] == '\n') {
		(*buf)[bytes - 1] = '\0';
		bytes--;
	}

	*len_out = bytes;

out:
	if (isatty(fileno(fd))) {
		/* reset the teminal */
		(void) tcsetattr(fileno(fd), TCSAFLUSH, &old_term);
		(void) sigaction(SIGINT, &osigint, NULL);
		(void) sigaction(SIGTSTP, &osigtstp, NULL);

		/* if we caught a signal, re-throw it now */
		if (caught_interrupt != 0) {
			(void) kill(getpid(), caught_interrupt);
		}

		/* print the newline that was not echo'd */
		printf("\n");
	}

	return (ret);

}

/*
 * Attempts to fetch key material, no matter where it might live. The key
 * material is allocated and returned in km_out. *can_retry_out will be set
 * to B_TRUE if the user is providing the key material interactively, allowing
 * for re-entry attempts.
 */
static int
get_key_material(libzfs_handle_t *hdl, boolean_t do_verify, boolean_t newkey,
    zfs_keyformat_t keyformat, char *keylocation, const char *fsname,
    uint8_t **km_out, size_t *kmlen_out, boolean_t *can_retry_out)
{
	int ret, i;
	zfs_keylocation_t keyloc = ZFS_KEYLOCATION_NONE;
	FILE *fd = NULL;
	uint8_t *km = NULL, *km2 = NULL;
	size_t kmlen, kmlen2;
	boolean_t can_retry = B_FALSE;

	/* verify and parse the keylocation */
	keyloc = zfs_prop_parse_keylocation(keylocation);

	/* open the appropriate file descriptor */
	switch (keyloc) {
	case ZFS_KEYLOCATION_PROMPT:
		fd = stdin;
		if (isatty(fileno(fd))) {
			can_retry = B_TRUE;

			/* raw keys cannot be entered on the terminal */
			if (keyformat == ZFS_KEYFORMAT_RAW) {
				ret = EINVAL;
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "Cannot enter raw keys on the terminal"));
				goto error;
			}
		}
		break;
	case ZFS_KEYLOCATION_URI:
		fd = fopen(&keylocation[7], "r");
		if (!fd) {
			ret = errno;
			errno = 0;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Failed to open key material file"));
			goto error;
		}
		break;
	default:
		ret = EINVAL;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Invalid keylocation."));
		goto error;
	}

	/* fetch the key material into the buffer */
	ret = get_key_material_raw(fd, fsname, keyformat, B_FALSE, newkey,
	    &km, &kmlen);
	if (ret != 0)
		goto error;

	/* do basic validation of the key material */
	switch (keyformat) {
	case ZFS_KEYFORMAT_RAW:
		/* verify the key length is correct */
		if (kmlen < WRAPPING_KEY_LEN) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Raw key too short (expected %u)."),
			    WRAPPING_KEY_LEN);
			goto error;
		}

		if (kmlen > WRAPPING_KEY_LEN) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Raw key too long (expected %u)."),
			    WRAPPING_KEY_LEN);
			goto error;
		}
		break;
	case ZFS_KEYFORMAT_HEX:
		/* verify the key length is correct */
		if (kmlen < WRAPPING_KEY_LEN * 2) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Hex key too short (expected %u)."),
			    WRAPPING_KEY_LEN * 2);
			goto error;
		}

		if (kmlen > WRAPPING_KEY_LEN * 2) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Hex key too long (expected %u)."),
			    WRAPPING_KEY_LEN * 2);
			goto error;
		}

		/* check for invalid hex digits */
		for (i = 0; i < WRAPPING_KEY_LEN * 2; i++) {
			if (!isxdigit((char)km[i])) {
				ret = EINVAL;
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "Invalid hex character detected."));
				goto error;
			}
		}
		break;
	case ZFS_KEYFORMAT_PASSPHRASE:
		/* verify the length is correct */
		if (kmlen > MAX_PASSPHRASE_LEN) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Passphrase too long (max 64)."));
			goto error;
		}

		if (kmlen < MIN_PASSPHRASE_LEN) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Passphrase too short (min 8)."));
			goto error;
		}
		break;
	default:
		/* can't happen */
		break;
	}

	if (do_verify && isatty(fileno(fd))) {
		ret = get_key_material_raw(fd, fsname, keyformat, B_TRUE,
		    newkey, &km2, &kmlen2);
		if (ret != 0)
			goto error;

		if (kmlen2 != kmlen ||
		    (memcmp((char *)km, (char *)km2, kmlen) != 0)) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Provided keys do not match."));
			goto error;
		}
	}

	if (fd != stdin)
		fclose(fd);

	if (km2 != NULL)
		free(km2);

	*km_out = km;
	*kmlen_out = kmlen;
	if (can_retry_out != NULL)
		*can_retry_out = can_retry;

	return (0);

error:
	if (km != NULL)
		free(km);

	if (km2 != NULL)
		free(km2);

	if (fd != NULL && fd != stdin)
		fclose(fd);

	*km_out = NULL;
	*kmlen_out = 0;
	if (can_retry_out != NULL)
		*can_retry_out = can_retry;

	return (ret);
}

static int
pbkdf2(uint8_t *passphrase, size_t passphraselen, uint8_t *salt,
    size_t saltlen, uint64_t iterations, uint8_t *output,
    size_t outputlen)
{
	int ret;
	uint64_t iter;
	uint32_t blockptr, i;
	uint16_t hmac_key_len;
	uint8_t *hmac_key;
	uint8_t block[SHA1_DIGEST_LEN * 2];
	uint8_t *hmacresult = block + SHA1_DIGEST_LEN;
	crypto_mechanism_t mech;
	crypto_key_t key;
	crypto_data_t in_data, out_data;
	crypto_ctx_template_t tmpl = NULL;

	/* initialize output */
	memset(output, 0, outputlen);

	/* initialize icp for use */
	thread_init();
	icp_init();

	/* HMAC key size is max(sizeof(uint32_t) + salt len, sha 256 len) */
	if (saltlen > SHA1_DIGEST_LEN) {
		hmac_key_len = saltlen + sizeof (uint32_t);
	} else {
		hmac_key_len = SHA1_DIGEST_LEN;
	}

	hmac_key = calloc(hmac_key_len, 1);
	if (!hmac_key) {
		ret = ENOMEM;
		goto error;
	}

	/* initialize sha 256 hmac mechanism */
	mech.cm_type = crypto_mech2id(SUN_CKM_SHA1_HMAC);
	mech.cm_param = NULL;
	mech.cm_param_len = 0;

	/* initialize passphrase as a crypto key */
	key.ck_format = CRYPTO_KEY_RAW;
	key.ck_length = BYTES_TO_BITS(passphraselen);
	key.ck_data = passphrase;

	/*
	 * initialize crypto data for the input data. length will change
	 * after the first iteration, so we will initialize it in the loop.
	 */
	in_data.cd_format = CRYPTO_DATA_RAW;
	in_data.cd_offset = 0;
	in_data.cd_raw.iov_base = (char *)hmac_key;

	/* initialize crypto data for the output data */
	out_data.cd_format = CRYPTO_DATA_RAW;
	out_data.cd_offset = 0;
	out_data.cd_length = SHA1_DIGEST_LEN;
	out_data.cd_raw.iov_base = (char *)hmacresult;
	out_data.cd_raw.iov_len = SHA1_DIGEST_LEN;

	/* initialize the context template */
	ret = crypto_create_ctx_template(&mech, &key, &tmpl, KM_SLEEP);
	if (ret != CRYPTO_SUCCESS) {
		ret = EIO;
		goto error;
	}

	/* main loop */
	for (blockptr = 0; blockptr < outputlen; blockptr += SHA1_DIGEST_LEN) {

		/*
		 * for the first iteration, the HMAC key is the user-provided
		 * salt concatenated with the block index (1-indexed)
		 */
		i = htobe32(1 + (blockptr / SHA1_DIGEST_LEN));
		memmove(hmac_key, salt, saltlen);
		memmove(hmac_key + saltlen, (uint8_t *)(&i), sizeof (uint32_t));

		/* block initializes to zeroes (no XOR) */
		memset(block, 0, SHA1_DIGEST_LEN);

		for (iter = 0; iter < iterations; iter++) {
			if (iter > 0) {
				in_data.cd_length = SHA1_DIGEST_LEN;
				in_data.cd_raw.iov_len = SHA1_DIGEST_LEN;
			} else {
				in_data.cd_length = saltlen + sizeof (uint32_t);
				in_data.cd_raw.iov_len =
				    saltlen + sizeof (uint32_t);
			}

			ret = crypto_mac(&mech, &in_data, &key, tmpl,
			    &out_data, NULL);
			if (ret != CRYPTO_SUCCESS) {
				ret = EIO;
				goto error;
			}

			/* HMAC key now becomes the output of this iteration */
			memmove(hmac_key, hmacresult, SHA1_DIGEST_LEN);

			/* XOR this iteration's result with the current block */
			for (i = 0; i < SHA1_DIGEST_LEN; i++) {
				block[i] ^= hmacresult[i];
			}
		}

		/*
		 * compute length of this block, make sure we don't write
		 * beyond the end of the output, truncating if necessary
		 */
		if (blockptr + SHA1_DIGEST_LEN > outputlen) {
			memmove(output + blockptr, block, outputlen - blockptr);
		} else {
			memmove(output + blockptr, block, SHA1_DIGEST_LEN);
		}
	}

	crypto_destroy_ctx_template(tmpl);
	free(hmac_key);
	icp_fini();
	thread_fini();

	return (0);

error:
	crypto_destroy_ctx_template(tmpl);
	if (hmac_key != NULL)
		free(hmac_key);
	icp_fini();
	thread_fini();

	return (ret);
}

static int
derive_key(libzfs_handle_t *hdl, zfs_keyformat_t format, uint64_t iters,
    uint8_t *key_material, size_t key_material_len, uint64_t salt,
    uint8_t **key_out)
{
	int ret;
	uint8_t *key;

	*key_out = NULL;

	key = zfs_alloc(hdl, WRAPPING_KEY_LEN);
	if (!key)
		return (ENOMEM);

	switch (format) {
	case ZFS_KEYFORMAT_RAW:
		bcopy(key_material, key, WRAPPING_KEY_LEN);
		break;
	case ZFS_KEYFORMAT_HEX:
		ret = hex_key_to_raw((char *)key_material,
		    WRAPPING_KEY_LEN * 2, key);
		if (ret != 0) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Invalid hex key provided."));
			goto error;
		}
		break;
	case ZFS_KEYFORMAT_PASSPHRASE:
		salt = LE_64(salt);
		ret = pbkdf2(key_material, strlen((char *)key_material),
		    ((uint8_t *)&salt), sizeof (uint64_t), iters,
		    key, WRAPPING_KEY_LEN);
		if (ret != 0) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Failed to generate key from passphrase."));
			goto error;
		}
		break;
	default:
		ret = EINVAL;
		goto error;
	}

	*key_out = key;
	return (0);

error:
	free(key);

	*key_out = NULL;
	return (ret);
}

static boolean_t
encryption_feature_is_enabled(zpool_handle_t *zph)
{
	nvlist_t *features;
	uint64_t feat_refcount;

	/* check that features can be enabled */
	if (zpool_get_prop_int(zph, ZPOOL_PROP_VERSION, NULL)
	    < SPA_VERSION_FEATURES)
		return (B_FALSE);

	/* check for crypto feature */
	features = zpool_get_features(zph);
	if (!features || nvlist_lookup_uint64(features,
	    spa_feature_table[SPA_FEATURE_ENCRYPTION].fi_guid,
	    &feat_refcount) != 0)
		return (B_FALSE);

	return (B_TRUE);
}

static int
populate_create_encryption_params_nvlists(libzfs_handle_t *hdl,
    zfs_handle_t *zhp, boolean_t newkey, zfs_keyformat_t keyformat,
    char *keylocation, nvlist_t *props, uint8_t **wkeydata, uint_t *wkeylen)
{
	int ret;
	uint64_t iters = 0, salt = 0;
	uint8_t *key_material = NULL;
	size_t key_material_len = 0;
	uint8_t *key_data = NULL;
	const char *fsname = (zhp) ? zfs_get_name(zhp) : NULL;

	/* get key material from keyformat and keylocation */
	ret = get_key_material(hdl, B_TRUE, newkey, keyformat, keylocation,
	    fsname, &key_material, &key_material_len, NULL);
	if (ret != 0)
		goto error;

	/* passphrase formats require a salt and pbkdf2 iters property */
	if (keyformat == ZFS_KEYFORMAT_PASSPHRASE) {
		/* always generate a new salt */
		random_init();
		ret = random_get_bytes((uint8_t *)&salt, sizeof (uint64_t));
		if (ret != 0) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Failed to generate salt."));
			goto error;
		}
		random_fini();

		ret = nvlist_add_uint64(props,
		    zfs_prop_to_name(ZFS_PROP_PBKDF2_SALT), salt);
		if (ret != 0) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Failed to add salt to properties."));
			goto error;
		}

		/*
		 * If not otherwise specified, use the default number of
		 * pbkdf2 iterations. If specified, we have already checked
		 * that the given value is greater than MIN_PBKDF2_ITERATIONS
		 * during zfs_valid_proplist().
		 */
		ret = nvlist_lookup_uint64(props,
		    zfs_prop_to_name(ZFS_PROP_PBKDF2_ITERS), &iters);
		if (ret == ENOENT) {
			iters = DEFAULT_PBKDF2_ITERATIONS;
			ret = nvlist_add_uint64(props,
			    zfs_prop_to_name(ZFS_PROP_PBKDF2_ITERS), iters);
			if (ret != 0)
				goto error;
		} else if (ret != 0) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Failed to get pbkdf2 iterations."));
			goto error;
		}
	} else {
		/* check that pbkdf2iters was not specified by the user */
		ret = nvlist_lookup_uint64(props,
		    zfs_prop_to_name(ZFS_PROP_PBKDF2_ITERS), &iters);
		if (ret == 0) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Cannot specify pbkdf2iters with a non-passphrase "
			    "keyformat."));
			goto error;
		}
	}

	/* derive a key from the key material */
	ret = derive_key(hdl, keyformat, iters, key_material, key_material_len,
	    salt, &key_data);
	if (ret != 0)
		goto error;

	free(key_material);

	*wkeydata = key_data;
	*wkeylen = WRAPPING_KEY_LEN;
	return (0);

error:
	if (key_material != NULL)
		free(key_material);
	if (key_data != NULL)
		free(key_data);

	*wkeydata = NULL;
	*wkeylen = 0;
	return (ret);
}

static boolean_t
proplist_has_encryption_props(nvlist_t *props)
{
	int ret;
	uint64_t intval;
	char *strval;

	ret = nvlist_lookup_uint64(props,
	    zfs_prop_to_name(ZFS_PROP_ENCRYPTION), &intval);
	if (ret == 0 && intval != ZIO_CRYPT_OFF)
		return (B_TRUE);

	ret = nvlist_lookup_string(props,
	    zfs_prop_to_name(ZFS_PROP_KEYLOCATION), &strval);
	if (ret == 0 && strcmp(strval, "none") != 0)
		return (B_TRUE);

	ret = nvlist_lookup_uint64(props,
	    zfs_prop_to_name(ZFS_PROP_KEYFORMAT), &intval);
	if (ret == 0)
		return (B_TRUE);

	ret = nvlist_lookup_uint64(props,
	    zfs_prop_to_name(ZFS_PROP_PBKDF2_ITERS), &intval);
	if (ret == 0)
		return (B_TRUE);

	return (B_FALSE);
}

int
zfs_crypto_is_encryption_root(zfs_handle_t *zhp, boolean_t *enc_root)
{
	int ret;
	char prop_keylocation[MAXNAMELEN];
	char keylocation_src[MAXNAMELEN];
	zprop_source_t keylocation_srctype;

	/* if the dataset isn't encrypted, just return */
	if (zfs_prop_get_int(zhp, ZFS_PROP_ENCRYPTION) == ZIO_CRYPT_OFF) {
		*enc_root = B_FALSE;
		return (0);
	}

	/* fetch the keylocation and its source */
	ret = zfs_prop_get(zhp, ZFS_PROP_KEYLOCATION, prop_keylocation,
	    sizeof (prop_keylocation), &keylocation_srctype, keylocation_src,
	    sizeof (keylocation_src), B_TRUE);
	if (ret != 0) {
		*enc_root = B_FALSE;
		return (ret);
	}

	/* check if the keylocation was inheritted */
	if (keylocation_srctype == ZPROP_SRC_INHERITED) {
		*enc_root = B_FALSE;
		return (0);
	}

	*enc_root = B_TRUE;
	return (0);
}

int
zfs_crypto_create(libzfs_handle_t *hdl, char *parent_name, nvlist_t *props,
    nvlist_t *pool_props, uint8_t **wkeydata_out, uint_t *wkeylen_out)
{
	int ret;
	char errbuf[1024];
	uint64_t crypt = ZIO_CRYPT_INHERIT, pcrypt = ZIO_CRYPT_INHERIT;
	uint64_t keyformat = ZFS_KEYFORMAT_NONE;
	char *keylocation = NULL;
	zfs_handle_t *pzhp = NULL;
	uint8_t *wkeydata = NULL;
	uint_t wkeylen = 0;
	boolean_t local_crypt = B_TRUE;

	(void) snprintf(errbuf, sizeof (errbuf),
	    dgettext(TEXT_DOMAIN, "Encryption create error"));

	/* lookup crypt from props */
	ret = nvlist_lookup_uint64(props,
	    zfs_prop_to_name(ZFS_PROP_ENCRYPTION), &crypt);
	if (ret != 0)
		local_crypt = B_FALSE;

	/* lookup key location and format from props */
	(void) nvlist_lookup_uint64(props,
	    zfs_prop_to_name(ZFS_PROP_KEYFORMAT), &keyformat);
	(void) nvlist_lookup_string(props,
	    zfs_prop_to_name(ZFS_PROP_KEYLOCATION), &keylocation);

	if (parent_name != NULL) {
		/* get a reference to parent dataset */
		pzhp = make_dataset_handle(hdl, parent_name);
		if (pzhp == NULL) {
			ret = ENOENT;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Failed to lookup parent."));
			goto out;
		}

		/* Lookup parent's crypt */
		pcrypt = zfs_prop_get_int(pzhp, ZFS_PROP_ENCRYPTION);

		/* Params require the encryption feature */
		if (!encryption_feature_is_enabled(pzhp->zpool_hdl)) {
			if (proplist_has_encryption_props(props)) {
				ret = EINVAL;
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "Encryption feature not enabled."));
				goto out;
			}

			ret = 0;
			goto out;
		}
	} else {
		/*
		 * special case for root dataset where encryption feature
		 * feature won't be on disk yet
		 */
		if (!nvlist_exists(pool_props, "feature@encryption")) {
			if (proplist_has_encryption_props(props)) {
				ret = EINVAL;
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "Encryption feature not enabled."));
				goto out;
			}

			ret = 0;
			goto out;
		}

		pcrypt = ZIO_CRYPT_OFF;
	}

	/* Check for encryption being explicitly truned off */
	if (crypt == ZIO_CRYPT_OFF && pcrypt != ZIO_CRYPT_OFF) {
		ret = EINVAL;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Invalid encryption value. Dataset must be encrypted."));
		goto out;
	}

	/* Get the inherited encryption property if we don't have it locally */
	if (!local_crypt)
		crypt = pcrypt;

	/*
	 * At this point crypt should be the actual encryption value. If
	 * encryption is off just verify that no encryption properties have
	 * been specified and return.
	 */
	if (crypt == ZIO_CRYPT_OFF) {
		if (proplist_has_encryption_props(props)) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Encryption must be turned on to set encryption "
			    "properties."));
			goto out;
		}

		ret = 0;
		goto out;
	}

	/*
	 * If we have a parent crypt it is valid to specify encryption alone.
	 * This will result in a child that is encrypted with the chosen
	 * encryption suite that will also inherit the parent's key. If
	 * the parent is not encrypted we need an encryption suite provided.
	 */
	if (pcrypt == ZIO_CRYPT_OFF && keylocation == NULL &&
	    keyformat == ZFS_KEYFORMAT_NONE) {
		ret = EINVAL;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Keyformat required for new encryption root."));
		goto out;
	}

	/*
	 * Specifying a keylocation implies this will be a new encryption root.
	 * Check that a keyformat is also specified.
	 */
	if (keylocation != NULL && keyformat == ZFS_KEYFORMAT_NONE) {
		ret = EINVAL;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Keyformat required for new encryption root."));
		goto out;
	}

	/* default to prompt if no keylocation is specified */
	if (keyformat != ZFS_KEYFORMAT_NONE && keylocation == NULL) {
		keylocation = "prompt";
		ret = nvlist_add_string(props,
		    zfs_prop_to_name(ZFS_PROP_KEYLOCATION), keylocation);
		if (ret != 0)
			goto out;
	}

	/*
	 * If a local key format is provided, this dataset will be a new
	 * encryption root. Populate the encryption params.
	 */
	if (keylocation != NULL) {
		ret = populate_create_encryption_params_nvlists(hdl, NULL,
		    B_FALSE, keyformat, keylocation, props, &wkeydata,
		    &wkeylen);
		if (ret != 0)
			goto out;
	}

	if (pzhp != NULL)
		zfs_close(pzhp);

	*wkeydata_out = wkeydata;
	*wkeylen_out = wkeylen;
	return (0);

out:
	if (pzhp != NULL)
		zfs_close(pzhp);
	if (wkeydata != NULL)
		free(wkeydata);

	*wkeydata_out = NULL;
	*wkeylen_out = 0;
	return (ret);
}

int
zfs_crypto_clone(libzfs_handle_t *hdl, zfs_handle_t *origin_zhp,
    char *parent_name, nvlist_t *props, uint8_t **wkeydata_out,
    uint_t *wkeylen_out)
{
	int ret;
	char errbuf[1024];
	uint64_t keyformat = ZFS_KEYFORMAT_NONE;
	char *keylocation = NULL;
	uint8_t *wkeydata = NULL;
	uint_t wkeylen = 0;
	zfs_handle_t *pzhp = NULL;
	uint64_t crypt, pcrypt, ocrypt, okey_status;

	(void) snprintf(errbuf, sizeof (errbuf),
	    dgettext(TEXT_DOMAIN, "Encryption clone error"));

	/* get a reference to parent dataset, should never be NULL */
	pzhp = make_dataset_handle(hdl, parent_name);
	if (pzhp == NULL) {
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Failed to lookup parent."));
		return (ENOENT);
	}

	/* Lookup parent's crypt */
	pcrypt = zfs_prop_get_int(pzhp, ZFS_PROP_ENCRYPTION);
	ocrypt = zfs_prop_get_int(origin_zhp, ZFS_PROP_ENCRYPTION);

	/* lookup keylocation from props */
	(void) nvlist_lookup_uint64(props,
	    zfs_prop_to_name(ZFS_PROP_KEYFORMAT), &keyformat);
	(void) nvlist_lookup_string(props,
	    zfs_prop_to_name(ZFS_PROP_KEYLOCATION), &keylocation);

	/* encryption should not be set since it must match the origin */
	ret = nvlist_lookup_uint64(props, zfs_prop_to_name(ZFS_PROP_ENCRYPTION),
	    &crypt);
	if (ret == 0) {
		ret = EINVAL;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Encryption may not be specified during cloning."));
		goto out;
	}

	/* all children of encrypted parents must be encrypted */
	if (pcrypt != ZIO_CRYPT_OFF && ocrypt == ZIO_CRYPT_OFF) {
		ret = EINVAL;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Cannot create unencrypted clone as child "
		    "of encrypted parent."));
		goto out;
	}

	/*
	 * If this dataset won't be encrypted check to ensure no encryption
	 * params were set and return.
	 */
	if (ocrypt == ZIO_CRYPT_OFF) {
		if (proplist_has_encryption_props(props)) {
			ret = EINVAL;
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "Encryption properties may not be set "
			    "for an unencrypted clone."));
			goto out;
		}

		ret = 0;
		goto out;
	}

	/*
	 * Specifying a keylocation implies this will be a new encryption root.
	 * Check that a keyformat is also specified.
	 */
	if (keylocation != NULL && keyformat == ZFS_KEYFORMAT_NONE) {
		ret = EINVAL;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Keyformat required for new encryption root."));
		goto out;
	}

	/* default to prompt if no keylocation is specified */
	if (keyformat != ZFS_KEYFORMAT_NONE && keylocation == NULL) {
		keylocation = "prompt";
		ret = nvlist_add_string(props,
		    zfs_prop_to_name(ZFS_PROP_KEYLOCATION), keylocation);
		if (ret != 0)
			goto out;
	}

	/*
	 * By this point this dataset will be encrypted. The origin's
	 * wrapping key must be loaded
	 */
	okey_status = zfs_prop_get_int(origin_zhp, ZFS_PROP_KEYSTATUS);
	if (okey_status != ZFS_KEYSTATUS_AVAILABLE) {
		ret = EACCES;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Origin wrapping key must be loaded."));
		goto out;
	}

	/*
	 * if the parent doesn't have a key format to inherit we need
	 * one provided for us
	 */
	if (pcrypt == ZIO_CRYPT_OFF && keyformat == ZFS_KEYFORMAT_NONE) {
		ret = EINVAL;
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "Keyformat required."));
		goto out;
	}

	/* prepare the key if needed */
	if (keylocation != NULL) {
		ret = populate_create_encryption_params_nvlists(hdl, NULL,
		    B_FALSE, keyformat, keylocation, props, &wkeydata,
		    &wkeylen);
		if (ret != 0)
			goto out;
	}

	zfs_close(pzhp);

	*wkeydata_out = wkeydata;
	*wkeylen_out = wkeylen;
	return (0);

out:
	if (pzhp != NULL)
		zfs_close(pzhp);
	if (wkeydata != NULL)
		free(wkeydata);

	*wkeydata_out = NULL;
	*wkeylen_out = 0;
	return (ret);
}

typedef struct loadkeys_cbdata {
	uint64_t cb_numfailed;
	uint64_t cb_numattempted;
} loadkey_cbdata_t;

static int
load_keys_cb(zfs_handle_t *zhp, void *arg)
{
	int ret;
	boolean_t is_encroot;
	loadkey_cbdata_t *cb = arg;
	uint64_t keystatus = zfs_prop_get_int(zhp, ZFS_PROP_KEYSTATUS);

	/* only attempt to load keys for encryption roots */
	ret = zfs_crypto_is_encryption_root(zhp, &is_encroot);
	if (ret != 0 || !is_encroot)
		goto out;

	/* don't attempt to load already loaded keys */
	if (keystatus == ZFS_KEYSTATUS_AVAILABLE)
		goto out;

	/* Attempt to load the key. Record status in cb. */
	cb->cb_numattempted++;

	ret = zfs_crypto_load_key(zhp, B_FALSE, NULL);
	if (ret)
		cb->cb_numfailed++;

out:
	(void) zfs_iter_filesystems(zhp, load_keys_cb, cb);
	zfs_close(zhp);

	/* always return 0, since this function is best effort */
	return (0);
}

/*
 * This function is best effort. It attempts to load all the keys for the given
 * filesystem and all of its children.
 */
int
zfs_crypto_attempt_load_keys(libzfs_handle_t *hdl, char *fsname)
{
	int ret;
	zfs_handle_t *zhp = NULL;
	loadkey_cbdata_t cb = { 0 };

	zhp = zfs_open(hdl, fsname, ZFS_TYPE_FILESYSTEM | ZFS_TYPE_VOLUME);
	if (zhp == NULL) {
		ret = ENOENT;
		goto error;
	}

	ret = load_keys_cb(zfs_handle_dup(zhp), &cb);
	if (ret)
		goto error;

	(void) printf(gettext("%llu / %llu keys successfully loaded\n"),
	    (u_longlong_t)(cb.cb_numattempted - cb.cb_numfailed),
	    (u_longlong_t)cb.cb_numattempted);

	if (cb.cb_numfailed != 0) {
		ret = -1;
		goto error;
	}

	zfs_close(zhp);
	return (0);

error:
	if (zhp != NULL)
		zfs_close(zhp);
	return (ret);
}

int
zfs_crypto_load_key(zfs_handle_t *zhp, boolean_t noop, char *alt_keylocation)
{
	int ret, attempts = 0;
	char errbuf[1024];
	uint64_t keystatus, iters = 0, salt = 0;
	uint64_t keyformat = ZFS_KEYFORMAT_NONE;
	char prop_keylocation[MAXNAMELEN];
	char keylocation_src[MAXNAMELEN];
	char *keylocation = NULL;
	uint8_t *key_material = NULL, *key_data = NULL;
	size_t key_material_len;
	zprop_source_t keylocation_srctype;
	boolean_t can_retry = B_FALSE, correctible = B_FALSE;

	(void) snprintf(errbuf, sizeof (errbuf),
	    dgettext(TEXT_DOMAIN, "Key load error"));

	/* check that encryption is enabled for the pool */
	if (!encryption_feature_is_enabled(zhp->zpool_hdl)) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Encryption feature not enabled."));
		ret = EINVAL;
		goto error;
	}

	/* Fetch the keyformat. Check that the dataset is encrypted. */
	keyformat = zfs_prop_get_int(zhp, ZFS_PROP_KEYFORMAT);
	if (keyformat == ZFS_KEYFORMAT_NONE) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "'%s' is not encrypted."), zfs_get_name(zhp));
		ret = EINVAL;
		goto error;
	}

	/*
	 * Fetch the key location. Check that we are working with an
	 * encryption root.
	 */
	ret = zfs_prop_get(zhp, ZFS_PROP_KEYLOCATION, prop_keylocation,
	    sizeof (prop_keylocation), &keylocation_srctype, keylocation_src,
	    sizeof (keylocation_src), B_TRUE);
	if (ret != 0) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Failed to get keylocation for '%s'."), zfs_get_name(zhp));
		goto error;
	} else if (keylocation_srctype == ZPROP_SRC_INHERITED) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Keys must be loaded for encryption root of '%s' (%s)."),
		    zfs_get_name(zhp), keylocation_src);
		ret = EINVAL;
		goto error;
	}

	/*
	 * if the caller has elected to override the keylocation property
	 * use that instead
	 */
	if (alt_keylocation != NULL)
		keylocation = alt_keylocation;
	else
		keylocation = prop_keylocation;

	/* check that the key is unloaded unless this is a noop */
	if (!noop) {
		keystatus = zfs_prop_get_int(zhp, ZFS_PROP_KEYSTATUS);
		if (keystatus == ZFS_KEYSTATUS_AVAILABLE) {
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Key already loaded for '%s'."), zfs_get_name(zhp));
			ret = EEXIST;
			goto error;
		}
	}

	/* passphrase formats require a salt and pbkdf2_iters property */
	if (keyformat == ZFS_KEYFORMAT_PASSPHRASE) {
		salt = zfs_prop_get_int(zhp, ZFS_PROP_PBKDF2_SALT);
		iters = zfs_prop_get_int(zhp, ZFS_PROP_PBKDF2_ITERS);
	}

try_again:
	/* fetching and deriving the key are correctible errors. set the flag */
	correctible = B_TRUE;

	/* get key material from key format and location */
	ret = get_key_material(zhp->zfs_hdl, B_FALSE, B_FALSE, keyformat,
	    keylocation, zfs_get_name(zhp), &key_material, &key_material_len,
	    &can_retry);
	if (ret != 0)
		goto error;

	/* derive a key from the key material */
	ret = derive_key(zhp->zfs_hdl, keyformat, iters, key_material,
	    key_material_len, salt, &key_data);
	if (ret != 0)
		goto error;

	correctible = B_FALSE;

	/* pass the wrapping key and noop flag to the ioctl */
	ret = lzc_load_key(zhp->zfs_name, noop, key_data, WRAPPING_KEY_LEN);
	if (ret != 0) {
		switch (ret) {
		case EINVAL:
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Invalid parameters provided for %s."),
			    zfs_get_name(zhp));
			break;
		case EEXIST:
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Key already loaded for '%s'."), zfs_get_name(zhp));
			break;
		case EBUSY:
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "'%s' is busy."), zfs_get_name(zhp));
			break;
		case EACCES:
			correctible = B_TRUE;
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Incorrect key provided for '%s'."),
			    zfs_get_name(zhp));
			break;
		}
		goto error;
	}

	free(key_material);
	free(key_data);

	return (0);

error:
	zfs_error(zhp->zfs_hdl, EZFS_CRYPTOFAILED, errbuf);
	if (key_material != NULL)
		free(key_material);
	if (key_data != NULL)
		free(key_data);

	/*
	 * Here we decide if it is ok to allow the user to retry entering their
	 * key. The can_retry flag will be set if the user is entering their
	 * key from an interactive prompt. The correctible flag will only be
	 * set if an error that occured could be corrected by retrying. Both
	 * flags are needed to allow the user to attempt key entry again
	 */
	if (can_retry && correctible && attempts <= MAX_KEY_PROMPT_ATTEMPTS) {
		attempts++;
		goto try_again;
	}

	return (ret);
}

int
zfs_crypto_unload_key(zfs_handle_t *zhp)
{
	int ret;
	char errbuf[1024];
	char prop_keylocation[MAXNAMELEN];
	char keylocation_src[MAXNAMELEN];
	uint64_t keystatus, keyformat;
	zprop_source_t keylocation_srctype;

	(void) snprintf(errbuf, sizeof (errbuf),
	    dgettext(TEXT_DOMAIN, "Key unload error"));

	/* check that encryption is enabled for the pool */
	if (!encryption_feature_is_enabled(zhp->zpool_hdl)) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Encryption feature not enabled."));
		ret = EINVAL;
		goto error;
	}

	/* Fetch the keyformat. Check that the dataset is encrypted. */
	keyformat = zfs_prop_get_int(zhp, ZFS_PROP_KEYFORMAT);
	if (keyformat == ZFS_KEYFORMAT_NONE) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "'%s' is not encrypted."), zfs_get_name(zhp));
		ret = EINVAL;
		goto error;
	}

	/*
	 * Fetch the key location. Check that we are working with an
	 * encryption root.
	 */
	ret = zfs_prop_get(zhp, ZFS_PROP_KEYLOCATION, prop_keylocation,
	    sizeof (prop_keylocation), &keylocation_srctype, keylocation_src,
	    sizeof (keylocation_src), B_TRUE);
	if (ret != 0) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Failed to get keylocation for '%s'."), zfs_get_name(zhp));
		goto error;
	} else if (keylocation_srctype == ZPROP_SRC_INHERITED) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Keys must be unloaded for encryption root of '%s' (%s)."),
		    zfs_get_name(zhp), keylocation_src);
		ret = EINVAL;
		goto error;
	}

	/* check that the key is loaded */
	keystatus = zfs_prop_get_int(zhp, ZFS_PROP_KEYSTATUS);
	if (keystatus == ZFS_KEYSTATUS_UNAVAILABLE) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Key already unloaded for '%s'."), zfs_get_name(zhp));
		ret = ENOENT;
		goto error;
	}

	/* call the ioctl */
	ret = lzc_unload_key(zhp->zfs_name);

	if (ret != 0) {
		switch (ret) {
		case ENOENT:
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Key already unloaded for '%s'."),
			    zfs_get_name(zhp));
			break;
		case EBUSY:
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "'%s' is busy."), zfs_get_name(zhp));
			break;
		}
		zfs_error(zhp->zfs_hdl, EZFS_CRYPTOFAILED, errbuf);
	}

	return (ret);

error:
	zfs_error(zhp->zfs_hdl, EZFS_CRYPTOFAILED, errbuf);
	return (ret);
}

static int
zfs_crypto_verify_rewrap_nvlist(zfs_handle_t *zhp, nvlist_t *props,
    nvlist_t **props_out, char *errbuf)
{
	int ret;
	nvpair_t *elem = NULL;
	zfs_prop_t prop;
	nvlist_t *new_props = NULL;

	new_props = fnvlist_alloc();

	/*
	 * loop through all provided properties, we should only have
	 * keyformat, keylocation and pbkdf2iters. The actual validation of
	 * values is done by zfs_valid_proplist().
	 */
	while ((elem = nvlist_next_nvpair(props, elem)) != NULL) {
		const char *propname = nvpair_name(elem);
		prop = zfs_name_to_prop(propname);

		switch (prop) {
		case ZFS_PROP_PBKDF2_ITERS:
		case ZFS_PROP_KEYFORMAT:
		case ZFS_PROP_KEYLOCATION:
			break;
		default:
			ret = EINVAL;
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Only keyformat, keylocation and pbkdf2iters may "
			    "be set with this command."));
			goto error;
		}
	}

	new_props = zfs_valid_proplist(zhp->zfs_hdl, zhp->zfs_type, props,
	    zfs_prop_get_int(zhp, ZFS_PROP_ZONED), NULL, zhp->zpool_hdl,
	    B_TRUE, errbuf);
	if (new_props == NULL)
		goto error;

	*props_out = new_props;
	return (0);

error:
	nvlist_free(new_props);
	*props_out = NULL;
	return (ret);
}

int
zfs_crypto_rewrap(zfs_handle_t *zhp, nvlist_t *raw_props, boolean_t inheritkey)
{
	int ret;
	char errbuf[1024];
	boolean_t is_encroot;
	nvlist_t *props = NULL;
	uint8_t *wkeydata = NULL;
	uint_t wkeylen = 0;
	uint64_t crypt, pcrypt, keystatus, pkeystatus;
	uint64_t keyformat = ZFS_KEYFORMAT_NONE;
	zfs_handle_t *pzhp = NULL;
	char *keylocation = NULL;
	char prop_keylocation[MAXNAMELEN];
	char parent_name[ZFS_MAX_DATASET_NAME_LEN];

	(void) snprintf(errbuf, sizeof (errbuf),
	    dgettext(TEXT_DOMAIN, "Key change error"));

	/* check that encryption is enabled for the pool */
	if (!encryption_feature_is_enabled(zhp->zpool_hdl)) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Encryption feature not enabled."));
		ret = EINVAL;
		goto error;
	}

	/* get crypt from dataset */
	crypt = zfs_prop_get_int(zhp, ZFS_PROP_ENCRYPTION);
	if (crypt == ZIO_CRYPT_OFF) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Dataset not encrypted."));
		ret = EINVAL;
		goto error;
	}

	/*
	 * if the user wants to use the inheritkey variant of this function
	 * we son't need to collect any crypto arguments
	 */
	if (!inheritkey) {
		/* validate the provided properties */
		ret = zfs_crypto_verify_rewrap_nvlist(zhp, raw_props, &props,
		    errbuf);
		if (ret != 0)
			goto error;

		/*
		 * Load keyformat and keylocation from the nvlist. Fetch from
		 * the dataset properties if not specified.
		 */
		(void) nvlist_lookup_uint64(props,
		    zfs_prop_to_name(ZFS_PROP_KEYFORMAT), &keyformat);
		(void) nvlist_lookup_string(props,
		    zfs_prop_to_name(ZFS_PROP_KEYLOCATION), &keylocation);

		if (keyformat == ZFS_KEYFORMAT_NONE)
			keyformat = zfs_prop_get_int(zhp, ZFS_PROP_KEYFORMAT);

		if (keylocation == NULL) {
			ret = zfs_prop_get(zhp, ZFS_PROP_KEYLOCATION,
			    prop_keylocation, sizeof (prop_keylocation),
			    NULL, NULL, 0, B_TRUE);
			if (ret != 0) {
				zfs_error_aux(zhp->zfs_hdl,
				    dgettext(TEXT_DOMAIN, "Failed to get "
				    "existing keylocation property."));
				goto error;
			}

			keylocation = prop_keylocation;
		}

		/* fetch the new wrapping key and associated properties */
		ret = populate_create_encryption_params_nvlists(zhp->zfs_hdl,
		    zhp, B_TRUE, keyformat, keylocation, props, &wkeydata,
		    &wkeylen);
		if (ret != 0)
			goto error;
	} else {
		/* check that zhp is an encryption root */
		ret = zfs_crypto_is_encryption_root(zhp, &is_encroot);
		if (ret != 0 || !is_encroot) {
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Key inheritting can only be performed on "
			    "encryption roots."));
			ret = EINVAL;
			goto error;
		}

		/* get the parent's name */
		ret = zfs_parent_name(zhp, parent_name, sizeof (parent_name));
		if (ret != 0) {
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Root dataset cannot inherit key."));
			ret = EINVAL;
			goto error;
		}

		/* get a handle to the parent */
		pzhp = make_dataset_handle(zhp->zfs_hdl, parent_name);
		if (pzhp == NULL) {
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Failed to lookup parent."));
			ret = ENOENT;
			goto error;
		}

		/* parent must be encrypted */
		pcrypt = zfs_prop_get_int(pzhp, ZFS_PROP_ENCRYPTION);
		if (pcrypt == ZIO_CRYPT_OFF) {
			zfs_error_aux(pzhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Parent must be encrypted."));
			ret = EINVAL;
			goto error;
		}

		/* check that the parent's key is loaded */
		pkeystatus = zfs_prop_get_int(pzhp, ZFS_PROP_KEYSTATUS);
		if (pkeystatus == ZFS_KEYSTATUS_UNAVAILABLE) {
			zfs_error_aux(pzhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Parent key must be loaded."));
			ret = EACCES;
			goto error;
		}
	}

	/* check that the key is loaded */
	keystatus = zfs_prop_get_int(zhp, ZFS_PROP_KEYSTATUS);
	if (keystatus == ZFS_KEYSTATUS_UNAVAILABLE) {
		zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
		    "Key must be loaded."));
		ret = EACCES;
		goto error;
	}

	/* call the ioctl */
	ret = lzc_change_key(zhp->zfs_name, props, wkeydata, wkeylen);
	if (ret != 0) {
		switch (ret) {
		case EINVAL:
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Invalid properties for key change."));
			break;
		case EACCES:
			zfs_error_aux(zhp->zfs_hdl, dgettext(TEXT_DOMAIN,
			    "Key is not currently loaded."));
			break;
		}
		zfs_error(zhp->zfs_hdl, EZFS_CRYPTOFAILED, errbuf);
	}

	if (pzhp != NULL)
		zfs_close(pzhp);
	if (props != NULL)
		nvlist_free(props);
	if (wkeydata != NULL)
		free(wkeydata);

	return (ret);

error:
	if (pzhp != NULL)
		zfs_close(pzhp);
	if (props != NULL)
		nvlist_free(props);
	if (wkeydata != NULL)
		free(wkeydata);

	zfs_error(zhp->zfs_hdl, EZFS_CRYPTOFAILED, errbuf);
	return (ret);
}
