#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <gsl/gsl_randist.h>
#include "../Evaluation/eval_lib.h"

#define MAX_SENTENCE_LENGTH 1000
#define MAX_STRING 100

#define STATUS_INTERVAL 15
const int EXP_LEN = 87;
const int vocab_hash_size = 30000000;  // Maximum 30 * 0.7 = 21M words in the vocabulary
const double epsilon = 1e-8;

struct vocab_word {
  long long cn;
  char *word;
};
struct vocab_word *vocab;
int min_count = 25;
float *exp_table;
float dim_penalty, log_dim_penalty, sparsity_weight;
float *input_embed, *context_embed;
long long embed_size, train_words;
const int table_size = 1e8;
long long vocab_max_size = 1000, vocab_size = 0;
int *table;
int *vocab_hash;
char read_vocab_file[MAX_STRING];
/*
  Build table which precompute exp function for certain integer
  values
*/
void build_exp_table() {
  exp_table = calloc(EXP_LEN * 2 + 1, sizeof(float));
  for (int i = -EXP_LEN; i <= EXP_LEN; i++) {
    exp_table[i + EXP_LEN] = exp(i);
  }
}

// Reads a single word from a file, assuming space + tab + EOL to be word boundaries
void ReadWord(char *word, FILE *fin) {
  int a = 0, ch;
  while (!feof(fin)) {
    ch = fgetc(fin);
    if (ch == 13) continue;
    if ((ch == ' ') || (ch == '\t') || (ch == '\n')) {
      if (a > 0) {
        if (ch == '\n') ungetc(ch, fin);
        break;
      }
      if (ch == '\n') {
        strcpy(word, (char *)"</s>");
        return;
      } else continue;
    }
    word[a] = ch;
    a++;
    if (a >= MAX_STRING - 1) a--;   // Truncate too long words
  }
  word[a] = 0;
}

// Returns hash value of a word
int GetWordHash(char *word) {
  unsigned long long a, hash = 0;
  for (a = 0; a < strlen(word); a++) hash = hash * 257 + word[a];
  hash = hash % vocab_hash_size;
  return hash;
}

// Returns position of a word in the vocabulary; if the word is not found, returns -1
int SearchVocab(char *word) {
  unsigned int hash = GetWordHash(word);
  while (1) {
    if (vocab_hash[hash] == -1) return -1;
    if (!strcmp(word, vocab[vocab_hash[hash]].word)) return vocab_hash[hash];
    hash = (hash + 1) % vocab_hash_size;
  }
  return -1;
}

// Reads a word and returns its index in the vocabulary
int ReadWordIndex(FILE *fin) {
  char word[MAX_STRING];
  ReadWord(word, fin);
  if (feof(fin)) return -1;
  return SearchVocab(word);
}

// Adds a word to the vocabulary
int AddWordToVocab(char *word) {
  unsigned int hash, length = strlen(word) + 1;
  if (length > MAX_STRING) length = MAX_STRING;
  vocab[vocab_size].word = (char *)calloc(length, sizeof(char));
  strcpy(vocab[vocab_size].word, word);
  vocab[vocab_size].cn = 0;
  vocab_size++;
  // Reallocate memory if needed
  if (vocab_size + 2 >= vocab_max_size) {
    vocab_max_size += 1000;
    vocab = (struct vocab_word *)realloc(vocab, vocab_max_size * sizeof(struct vocab_word));
  }
  hash = GetWordHash(word);
  while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
  vocab_hash[hash] = vocab_size - 1;
  return vocab_size - 1;
}

// Used later for sorting by word counts
int VocabCompare(const void *a, const void *b) {
  return ((struct vocab_word *)b)->cn - ((struct vocab_word *)a)->cn;
}

// Sorts the vocabulary by frequency using word counts
void SortVocab() {
  int a, size;
  unsigned int hash;
  // Sort the vocabulary and keep </s> at the first position
  qsort(&vocab[1], vocab_size - 1, sizeof(struct vocab_word), VocabCompare);
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  size = vocab_size;
  train_words = 0;
  for (a = 0; a < size; a++) {
    // Words occuring less than min_count times will be discarded from the vocab
    if ((vocab[a].cn < min_count) && (a != 0)) {
      vocab_size--;
      free(vocab[a].word);
    } else {
      // Hash will be re-computed, as after the sorting it is not actual
      hash=GetWordHash(vocab[a].word);
      while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
      vocab_hash[hash] = a;
      train_words += vocab[a].cn;
    }
  }
  vocab = (struct vocab_word *)realloc(vocab, (vocab_size + 1) * sizeof(struct vocab_word));
}

void ReadVocab() {
  long long a, i = 0;
  char c;
  char word[MAX_STRING];
  FILE *fin = fopen(read_vocab_file, "rb");
  if (fin == NULL) {
    printf("Vocabulary file not found\n");
    exit(1);
  }
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  vocab_size = 0;
  while (1) {
    ReadWord(word, fin);
    if (feof(fin)) break;
    a = AddWordToVocab(word);
    fscanf(fin, "%lld%c", &vocab[a].cn, &c);
    i++;
  }
  SortVocab();
}


// Build table from which to rand. sample words
void InitUnigramTable() {
  int a, i;
  double train_words_pow = 0;
  double d1, power = 0.75;
  table = (int *)malloc(table_size * sizeof(int));
  for (a = 0; a < vocab_size; a++) train_words_pow += pow(vocab[a].cn, power);
  i = 0;
  d1 = pow(vocab[i].cn, power) / train_words_pow;
  for (a = 0; a < table_size; a++) {
    table[a] = i;
    if (a / (double)table_size > d1) {
      i++;
      d1 += pow(vocab[i].cn, power) / train_words_pow;
    }
    if (i >= vocab_size) i = vocab_size - 1;
  }
}

/*
  Exp approximate function from 
  http://stackoverflow.com/questions/10552280/fast-exp-calculation-possible-to-improve-accuracy-without-losing-too-much-perfo/14143184#14143184
  Error in input range [-1,1] is 0.36%
*/
float exp_approx(float x) { 
  return (24+x*(24+x*(12+x*(4+x))))*0.041666666f;
}

/*
  Separate into integer and decimal components and use table and 
  approximate exp function to compute each part, respectively
*/
float exp_fast(float x) {
  int x_int = (int)x;
  float x_dec = x - x_int;

  float exp_table_val = 0.0;
  if (x_int < -EXP_LEN) {
    exp_table_val = exp_table[0];
  } 
  else if (x_int > EXP_LEN) {
    exp_table_val = exp_table[2*EXP_LEN];
  }
  else {
    exp_table_val = exp_table[x_int + EXP_LEN];
  }
 
  return exp_table_val * exp_approx(x_dec);   
}

/********************************************
  iSG Probability Functions (copied over but
  edited to not have infinte part)
********************************************/
float compute_z_dist(float *dist, long long w_idx, long long c_idx, int embed_size) { 
  float max_value = 0.0;
  for (int a = 0; a < embed_size; a++) {
    float val = -input_embed[w_idx + a]*context_embed[c_idx + a] 
                +log_dim_penalty + sparsity_weight*input_embed[w_idx + a]*input_embed[w_idx + a] 
                +sparsity_weight*context_embed[c_idx + a]*context_embed[c_idx+a];
    for (int b = a; b < embed_size; b++) {
      dist[b] += val;
    }
    if (-dist[a] > max_value)  max_value = -dist[a];
  }

  return max_value;
}

void compute_p_z_given_w_c(float *prob_z_given_w_c, float *sum_prob_z_given_w_c, long long w_idx,
  long long c_idx, int embed_size) {
  float max_value = compute_z_dist(prob_z_given_w_c, w_idx, c_idx, embed_size);
  float norm = 0.0;
  
  // iterate to exponeniate and compute norm
  for (int i = 0; i < embed_size; i++) {
    prob_z_given_w_c[i] = exp_fast(-prob_z_given_w_c[i] - max_value);
    norm += prob_z_given_w_c[i];
  }  

  // pre-calculate sums
  float sum = 0.0;
  for (int i = embed_size-1; i >= 0; i--) {
    sum += prob_z_given_w_c[i]/norm;
    sum_prob_z_given_w_c[i] = sum;
  }
}

// prob_c_z_given_w should be of size true_context_size * curr_z_plus_one
void compute_p_c_z_given_w(long long word, long long *context, float *prob_c_z_given_w, 
  float *sum_prob_c_z_given_w, int context_size, int embed_size) {
  // compute e^(-E(w,c,z)) for z = 1,...,curr_z,curr_z+1 for every context c
  long long w_idx = word * embed_size;
  float max_value = 0.0, norm = 0.0;

  for (int s = 0; s < context_size; s++) {
    long long c_idx = context[s] * embed_size;  
    float temp_value = compute_z_dist(prob_c_z_given_w + s * embed_size, w_idx, c_idx, embed_size); 
    if (temp_value > max_value)  temp_value = max_value;
  }
 
  // iterate to exponentiate and compute norm 
  for (int s = 0; s < context_size; s++) {
    for (int z = 0; z < embed_size; z++) {
      prob_c_z_given_w[s*embed_size + z] = exp_fast(-prob_c_z_given_w[s*embed_size + z] - max_value);
      norm += prob_c_z_given_w[s*embed_size + z];
    }
  }

  // compute prob and pre-calculate sums 
  for (int s = 0; s < context_size; s++) {
    float sum = 0;
    for (int z = embed_size-1; z >= 0; z--) {
      prob_c_z_given_w[s * embed_size + z] = prob_c_z_given_w[s * embed_size + z]/norm;
      sum += prob_c_z_given_w[s * embed_size + z];
      sum_prob_c_z_given_w[s * embed_size + z] = sum;
    }
  }
}
/********************************************
********************************************/

float get_log_prob(char *test_file_name, float *input_embed, float *context_embed, long embed_size) {
  int negative = 5;
  int window = 5; 
  unsigned long long next_random = 1;
  long long a, b, d, c, word, last_word, negative_word, sentence_length = 0, sentence_position = 0;
  long long sen[MAX_SENTENCE_LENGTH + 1];
  
  // terms needed for p(z|w,c)
  float *prob_z_given_w_c = (float *) calloc(embed_size, sizeof(float));
  float *sum_prob_z_given_w_c = (float *) calloc(embed_size, sizeof(float));
  // terms needed for p(c,z|w)
  float *prob_c_z_given_w = (float *) calloc(embed_size * (negative + 1), sizeof(float));
  float *sum_prob_c_z_given_w = (float *) calloc(embed_size * (negative + 1), sizeof(float));   
  // stores negative constext
  long long *neg_context = (long long *) calloc(negative, sizeof(long long)); 

  // Open corpus to read
  FILE *fi = fopen(test_file_name, "rb");
  fseek(fi, 0, SEEK_SET);
 
  float total_log_prob = 0.0;
  long iter = 0;
  while (1) {  
    // read a new sentence / line
    if (sentence_length == 0) {
      while (1) {
	word = ReadWordIndex(fi);
        if (feof(fi)) break;
	if (word == -1) continue;
	if (word == 0) break;
	sen[sentence_length] = word;
	sentence_length++;
	if (sentence_length >= MAX_SENTENCE_LENGTH) break;
      }
      sentence_position = 0;
    }

    // if EOF, break
    if ( sentence_length==0 && feof(fi) ) break;
 
    // start of test, get current word (w)
    word = sen[sentence_position];
    
    // MAIN LOOP THROUGH POSITIVE CONTEXT
    int pos_context_counter = 0; // size of positive context
    b = 0;
    float log_prob_current_context = 0.0;
    for (a = b; a < window * 2 + 1 - b; a++) if (a != window) {
      c = sentence_position - window + a;
      if (c < 0) continue; 
      if (c >= sentence_length) break;
      last_word = sen[c];
      if (last_word == -1) continue;
      pos_context_counter++;  

      // NEGATIVE SAMPLING CONTEXT WORDS
      d = negative;
      neg_context[0] = last_word;
      while (d>0) {
	neg_context[d] = 0; // clear old contexts
	next_random = next_random * (unsigned long long)25214903917 + 11;
	negative_word = table[(next_random >> 16) % table_size];
	if (negative_word == 0) negative_word = next_random % (vocab_size - 1) + 1;
	if (negative_word == word || negative_word <= 0) continue; 
	neg_context[d] = negative_word;
	d--;
      }

      compute_p_c_z_given_w(word, neg_context, prob_c_z_given_w, sum_prob_c_z_given_w, 
        negative+1, embed_size);

      float log_prob_c_w = log(sum_prob_c_z_given_w[0] + epsilon); 
      log_prob_current_context += log_prob_c_w; 
    }
    total_log_prob += log_prob_current_context;
    iter += pos_context_counter;
    if (iter % STATUS_INTERVAL == 0) printf("Iteration: %ld, Perplexity: %f\n", iter, exp(-total_log_prob/iter));
    fflush(stdout);

    sentence_position++;
    if (sentence_position >= sentence_length) {
      sentence_length = 0;
      continue;
    } 
  }

  free(neg_context);
  free(prob_z_given_w_c);
  free(sum_prob_z_given_w_c);
  free(prob_c_z_given_w);
  free(sum_prob_c_z_given_w);
  return exp(-total_log_prob/iter);
}

int main(int argc, char **argv) {
  char *vocab_local, *dummy_vocab;
  long long vocab_size_local = 0;
  char input_file_name[MAX_STRING], context_file_name[MAX_STRING], test_file_name[MAX_STRING];

  // Build exp table                                                                                                                                                                                                      
  build_exp_table();
  vocab = (struct vocab_word *)calloc(vocab_max_size, sizeof(struct vocab_word));
  vocab_hash = (int *)calloc(vocab_hash_size, sizeof(int));
  // Read arguments from command line                                                                                                                                                                                            
  strcpy(input_file_name, argv[1]);
  strcpy(context_file_name, argv[2]);
  strcpy(test_file_name, argv[3]);
  strcpy(read_vocab_file, argv[4]);
  sparsity_weight = atof(argv[5]);
  dim_penalty = atof(argv[6]);
  log_dim_penalty = log(dim_penalty);
  // log what we read in                                                                                                                                                                                           
  read_vectors(input_file_name, &vocab_size_local, &embed_size, &vocab_local, &input_embed);
  read_vectors(context_file_name, &vocab_size_local, &embed_size, &dummy_vocab, &context_embed);

  ReadVocab();
  InitUnigramTable();

  printf("Input vectors: %s\n", input_file_name);
  printf("Context vectors: %s\n", context_file_name);
  printf("Sparsity weight: %.8f\n", sparsity_weight);
  printf("Dimension penalty: %.8f\n", dim_penalty);
  printf("Embedding size: %lld\n", embed_size);
  fflush(stdout);
  
  printf("Starting testing...\n");
  float log_prob = get_log_prob(test_file_name, input_embed, context_embed, embed_size);
  free(exp_table);
  free(vocab);
  free(vocab_hash);
  printf("-----------------------------------\n");
  printf("Final Perplexity: %f\n", log_prob);
  fflush(stdout);
  return 0;
}

