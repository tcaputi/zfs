#include <sys/crypto/api.h>
#include <sys/crypto/common.h>

/* Placeholders so that libzpool will build */

crypto_mech_type_t crypto_mech2id(crypto_mech_name_t name){
	return 0;
}

int crypto_create_ctx_template(crypto_mechanism_t *mech, crypto_key_t *key, crypto_ctx_template_t *tmpl, int kmflag){
	return 0;
}

void crypto_destroy_ctx_template(crypto_ctx_template_t tmpl){
	return;
}

int crypto_encrypt(crypto_mechanism_t *mech, crypto_data_t *plaintext, crypto_key_t *key, crypto_ctx_template_t tmpl, crypto_data_t *ciphertext, crypto_call_req_t *cr){
	return 0;
}
	
int crypto_decrypt(crypto_mechanism_t *mech, crypto_data_t *ciphertext, crypto_key_t *key, crypto_ctx_template_t tmpl, crypto_data_t *plaintext, crypto_call_req_t *cr){
	return 0;
}