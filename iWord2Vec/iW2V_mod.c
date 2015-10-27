#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <gsl/gsl_randist.h>
#include "Evaluation/eval_lib.h"

// Global Variables
#define MAX_STRING 100
#define MAX_SENTENCE_LENGTH 1000

const int vocab_hash_size = 30000000;  // Maximum 30 * 0.7 = 21M words in the vocabulary
typedef float real;                    // Precision of float numbers

struct vocab_word {
  long long cn;
  char *word;
};

// pthread only allows passing of one argument
typedef struct {
  int id;
  bool expand;
} ThreadArg;

char train_file[MAX_STRING], output_file[MAX_STRING], context_output_file[MAX_STRING], fixed_dim_output_file[MAX_STRING], fixed_dim_init_file[MAX_STRING];
char save_vocab_file[MAX_STRING], read_vocab_file[MAX_STRING];
struct vocab_word *vocab;
int debug_mode = 2, window = 5, min_count = 1, num_threads = 1, min_reduce = 1;
real dim_penalty = 1.1;
int *vocab_hash;
long long vocab_max_size = 1000, vocab_size = 0, embed_max_size = 2000, embed_current_size = 5;
long long train_words = 0, word_count_actual = 0, iter = 5, fixed_dim_iter = 0, file_size = 0;
real alpha = 0.05, starting_alpha, sample = 1e-3, sparsity_weight = 0.001;
real *input_embed, *context_embed;
clock_t start;
int negative = 5;
int num_z_samples = 5;

const int table_size = 1e8;
const double epsilon = 1e-5;
int *table;

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

// Reduces the vocabulary by removing infrequent tokens
void ReduceVocab() {
  int a, b = 0;
  unsigned int hash;
  for (a = 0; a < vocab_size; a++) if (vocab[a].cn > min_reduce) {
      vocab[b].cn = vocab[a].cn;
      vocab[b].word = vocab[a].word;
      b++;
    } else free(vocab[a].word);
  vocab_size = b;
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  for (a = 0; a < vocab_size; a++) {
    // Hash will be re-computed, as it is not actual
    hash = GetWordHash(vocab[a].word);
    while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
    vocab_hash[hash] = a;
  }
  fflush(stdout);
  min_reduce++;
}

void LearnVocabFromTrainFile() {
  char word[MAX_STRING];
  FILE *fin;
  long long a, i;
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  fin = fopen(train_file, "rb");
  if (fin == NULL) {
    printf("ERROR: training data file not found!\n");
    exit(1);
  }
  vocab_size = 0;
  AddWordToVocab((char *)"</s>");
  while (1) {
    ReadWord(word, fin);
    if (feof(fin)) break;
    train_words++;
    if ((debug_mode > 1) && (train_words % 100000 == 0)) {
      printf("%lldK%c", train_words / 1000, 13);
      fflush(stdout);
    }
    i = SearchVocab(word);
    if (i == -1) {
      a = AddWordToVocab(word);
      vocab[a].cn = 1;
    } else vocab[i].cn++;
    if (vocab_size > vocab_hash_size * 0.7) ReduceVocab();
  }
  SortVocab();
  if (debug_mode > 0) {
    printf("Vocab size: %lld\n", vocab_size);
    printf("Words in train file: %lld\n", train_words);
  }
  file_size = ftell(fin);
  fclose(fin);
}

void SaveVocab() {
  long long i;
  FILE *fo = fopen(save_vocab_file, "wb");
  for (i = 0; i < vocab_size; i++) fprintf(fo, "%s %lld\n", vocab[i].word, vocab[i].cn);
  fclose(fo);
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
  if (debug_mode > 0) {
    printf("Vocab size: %lld\n", vocab_size);
    printf("Words in train file: %lld\n", train_words);
  }
  fin = fopen(train_file, "rb");
  if (fin == NULL) {
    printf("ERROR: training data file not found!\n");
    exit(1);
  }
  fseek(fin, 0, SEEK_END);
  file_size = ftell(fin);
  fclose(fin);
}

void InitNet() {
  long long a, b;
  unsigned long long next_random = 1;
  // initialize context embeddings
  a = posix_memalign((void **)&context_embed, 128, (long long)vocab_size * embed_max_size * sizeof(real));
  if (context_embed == NULL) {printf("Memory allocation failed\n"); exit(1);}
  for (a = 0; a < vocab_size; a++) for (b = 0; b < embed_max_size; b++) {
      // random (instead of zero) to avoid multi-threaded problems
      next_random = next_random * (unsigned long long)25214903917 + 11;
      context_embed[a * embed_max_size + b] = (((next_random & 0xFFFF) / (real)65536) - 0.5) / embed_current_size; 
  }
  // initialize input embeddings
  a = posix_memalign((void **)&input_embed, 128, (long long)vocab_size * embed_max_size * sizeof(real));
  for (a = 0; a < vocab_size; a++) for (b = 0; b < embed_max_size; b++) {
      // only initialize first few dims so we can tell the true vector length
      if (b < embed_current_size){
	next_random = next_random * (unsigned long long)25214903917 + 11;
	input_embed[a * embed_max_size + b] = (((next_random & 0xFFFF) / (real)65536) - 0.5) / embed_current_size;
      }
      else{
	input_embed[a * embed_max_size + b] = 0.0;
      }
    }
}

// function to compute E(w, c, z)
float compute_energy(long long w_idx, long long c_idx, int z){
  long long a;
  float energy = 0.0;
  for (a = 0; a<z; a++) energy += -input_embed[w_idx + a]*context_embed[c_idx + a] + log(dim_penalty) + sparsity_weight*pow(input_embed[w_idx + a],2) + sparsity_weight*pow(context_embed[c_idx + a],2);
  return energy;
}

/*
  Compute e^(-E(w,c,z)) for z=1,...,curr_z,curr_z+1  
  -> dist: float array to fill; should be of size curr_z+1 
  -> w_idx: word index
  -> c_idx: context index
  -> curr_z: current number of dimensions 
*/
void compute_z_dist(float *dist, long long w_idx, long long c_idx, int curr_z) { 
  for (int a = 0; a < curr_z; a++) {
    float val = -input_embed[w_idx + a]*context_embed[c_idx + a] + log(dim_penalty) + sparsity_weight*pow(input_embed[w_idx + a],2) + sparsity_weight*pow(context_embed[c_idx + a],2);
    for (int b = a; b <= curr_z; b++) {
      dist[b] += val;
    }
    dist[a] = exp(-dist[a]);
  }
  dist[curr_z] = (dim_penalty / (dim_penalty - 1.0)) * exp(-dist[curr_z]);
}

/* 
   Compute \sum_{c} e^(-E(w,c,z)) for each z=1,...,curr_z,curr_z+1; this array + norm represents p(z|w) 
   -> context: list of contexts c
   -> w_idx: position of word w 
   -> values_z_given_w: float array to fill
   -> true_context_size: number of contexts 
   -> curr_z: size of current dim l; l+1 is the dimension that could be added
*/
float compute_z_given_w(long long word, long long *context, float *values_z_given_w, int true_context_size, long long curr_z) {
  // compute e^(-E(w,c,z)) for z = 1,...,curr_z,curr_z+1 for every context c
  long long added_z = curr_z + 1;
  long long w_idx = word * embed_max_size;
  // create array of size 1 x (n_dims * n_context_words)
  float *z_dist_list = (float *) calloc(true_context_size * added_z, sizeof(float)); 
  for (int s = 0; s < true_context_size; s++) {
    long long c_idx = context[s] * embed_max_size;  
    compute_z_dist(z_dist_list + s * added_z, w_idx, c_idx, curr_z); 
  }
  // compute_z_dist should now have the prob. of each dim for every context word

  // debug
  /*printf("word=%s\n", vocab[word].word);
  for (int s = 0; s < true_context_size; s++) {
    printf("context=%d(%s)\n",  s, vocab[context[s]].word);
    for (int t = 0; t < added_z; t++) {
      printf("\tz=%d: %lf\n", t+1, z_dist_list[s*added_z + t]); 
    }
    printf("-------------------------\n");
  }*/
 
  float norm = 0;
  // sum across contexts for each z
  for (int z = 0; z < added_z; z++) {
    float sum = 0;
    for (int a = 0; a < true_context_size; a++) {
      sum += z_dist_list[a * added_z + z]; 
    }
    values_z_given_w[z] = sum;
    norm += sum;
  }
  free(z_dist_list);
  return norm;
}

// function to sample value of z_hat -- modified but essentially coppied from StackOverflow 
// http://stackoverflow.com/questions/25363450/generating-a-multinomial-distribution
int sample_from_mult(double probs[], int k, const gsl_rng* r){ // always sample 1 value
  unsigned int mult_op[k];
  gsl_ran_multinomial(r, k, 1, probs, mult_op);
  for (int idx=1; idx<=k; idx++){
    if (mult_op[idx-1]==1) return idx;
  }
  return 0;
}

// Samples N values of z_hat and returns in vals list and returns the mode of samples 
// Params:
// probs -- unnormalized probabilities
// k -- l+1 (size of current embedding + 1)
// N -- number of samples to draw
// vals -- N-size array containing the sampled values
// r -- random number generator 
int sample_from_mult_list(float float_probs[], int k, int vals[], int N, const gsl_rng* r) {
  unsigned int mult_op[k];
  // copy over to double array
  double probs[k];
  for (int i = 0; i < k; i++) {
    probs[i] = float_probs[i];
  }

  gsl_ran_multinomial(r, k, N, probs, mult_op); // returns array of size N with amount of each value sampled 
  int cnt = 0;
  // add sampled values to list 
  int max = 0, max_idx = -1;
  for (int idx=1; idx<=k; idx++) {
    //printf("z = %d : %d\n", idx-1, mult_op[idx-1]);
    if (max < mult_op[idx-1]) {
      max = mult_op[idx-1]; 
      max_idx = idx;
    }
    // We know amount of this value, so add this many to list
    for (int num=0; num<mult_op[idx-1]; num++) {
      vals[cnt] = idx;
      cnt++;
    }
  }
  return max_idx;
}

void debug_prob(float probs[], int len) {
  int i;
  printf("*****************\n");
  for (i = 0; i < len; ++i) {
    printf("z = %i prob: %f\n", i, probs[i]); 
  }
  printf("*****************\n");	
}

void print_args() {
  printf("# TRAINING SETTINGS #\n"); 
  printf("Train Corpus: %s\n", train_file);
  printf("Output file: %s\n", output_file);
  printf("Num. of threads: %d\n", num_threads);
  printf("Initial dimensionality: %lld\n", embed_current_size);
  printf("Max dimensionality: %lld\n", embed_max_size); 
  printf("Context window size: %d\n", window); 
  printf("Num. of negative samples: %d\n", negative); 
  printf("Training iterations (epochs): %lld\n", iter); 
  printf("Learning rate: %f\n", (float)alpha ); 
  printf("Dimension penalty: %f\n", (float)dim_penalty); 
  printf("Sparsity weight: %f\n", (float)sparsity_weight);
  if (fixed_dim_iter > 0) {
    printf("Fixed dim training iterations (epochs): %lld\n", fixed_dim_iter);
    printf("Fixed dim output file: %s\n", fixed_dim_output_file);
  }
  printf("#####################\n");  
}

void save_vectors(char *output_file, long long int vocab_size, long long int embed_current_size, struct vocab_word *vocab, real *input_embed) {
  FILE *fo;
  fo = fopen(output_file, "wb");
  // Save the word vectors
  fprintf(fo, "%lld %lld\n", vocab_size, embed_current_size);
  long a,b;
  for (a = 0; a < vocab_size; a++) {
    fprintf(fo, "%s ", vocab[a].word);
    // only print the non-zero dimensions
    for (b = 0; b < embed_current_size; b++) fprintf(fo, "%lf ", input_embed[a * embed_max_size + b]);
    fprintf(fo, "\n");
  }
  fclose(fo);
}

void *TrainModelThread(void *arg) {
  ThreadArg *thread_arg = (ThreadArg *)arg;
  int id = thread_arg->id;
  bool expand = thread_arg->expand;

  long long a, b, d, word, last_word, negative_word, sentence_length = 0, sentence_position = 0;
  long long word_count = 0, last_word_count = 0, sen[MAX_SENTENCE_LENGTH + 1], neg_samples[negative];
  long long input_word_position, context_word_position, negative_word_position, z_hat, c, local_iter = iter;
  unsigned long long next_random = (long long)id;
  clock_t now;
  FILE *fi = fopen(train_file, "rb");
  fseek(fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);
  // set up random number generator
  const gsl_rng_type * T2;
  gsl_rng * r2;
  srand(time(NULL));
  unsigned int Seed2 = rand();
  gsl_rng_env_setup();
  T2 = gsl_rng_default;
  r2 = gsl_rng_alloc (T2);
  gsl_rng_set (r2, Seed2);
 
  int *z_vals = (int *) calloc(num_z_samples, sizeof(int)); // sampled z values
  long long *pos_context_store = (long long *)calloc(2*window+1, sizeof(long long)); // stores positive context  
  float *unnormProbs_z_given_w = (float *) calloc(embed_max_size, sizeof(float));
  
  float *positive_context_fixed_sum_per_dim = (float *) calloc(embed_max_size, sizeof(float)); 
  float *positive_context_gradient = (float *) calloc((2*window+1) * embed_max_size, sizeof(float));
  float *prob_c_given_w_z = (float *) calloc((2*window+1), sizeof(float));

  float *input_gradient_accumulator = (float *)calloc(embed_max_size, sizeof(float));
  float *context_gradient_accumulator = (float *) calloc(embed_max_size, sizeof(float)); 
  float *neg_context_gradient_accumulator = (float *) calloc(negative * embed_max_size, sizeof(float));
  float *input_gradient = (float *) calloc(embed_max_size, sizeof(float));
  float *neg_probs = (float *) calloc(negative, sizeof(float)); // neg context probs
   
  float acc = 0.0; 
  while (1) {
    // track training progress
    if (word_count - last_word_count > 10000) {
      long long diff = word_count - last_word_count;
      word_count_actual += word_count - last_word_count;
      last_word_count = word_count;
      if ((debug_mode > 1)) {
        now=clock();
        printf("%cAlpha: %f  Progress: %.2f%%  Words/thread/sec: %.2fk  ", 13, alpha,
	       word_count_actual / (real)(iter * train_words + 1) * 100,
	       word_count_actual / ((real)(now - start + 1) / (real)CLOCKS_PER_SEC * 1000));
        printf("avg acc: %f  ", acc/diff);
	printf("curr dim: %lld\n", embed_current_size);	
        fflush(stdout);
        acc = 0.0;
      }
      alpha = starting_alpha * (1 - word_count_actual / (real)(iter * train_words + 1));
      if (alpha < starting_alpha * 0.0001) alpha = starting_alpha * 0.0001;
    }
    // read a new sentence / line
    if (sentence_length == 0) {
      while (1) {
        word = ReadWordIndex(fi);
        if (feof(fi)) break;
        if (word == -1) continue;
        word_count++;
        if (word == 0) break;
        // The subsampling randomly discards frequent words while keeping the ranking same
        if (sample > 0) {
          real ran = (sqrt(vocab[word].cn / (sample * train_words)) + 1) * (sample * train_words) / vocab[word].cn;
          next_random = next_random * (unsigned long long)25214903917 + 11;
          if (ran < (next_random & 0xFFFF) / (real)65536) continue;
        }
        sen[sentence_length] = word;
        sentence_length++;
        if (sentence_length >= MAX_SENTENCE_LENGTH) break;
      }
      sentence_position = 0;
    }
    // if EOF, reset to beginning
    if (feof(fi) || (word_count > train_words / num_threads)) {
      word_count_actual += word_count - last_word_count;
      local_iter--;
      if (local_iter == 0) break;
      word_count = 0;
      last_word_count = 0;
      sentence_length = 0;
      fseek(fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);
      continue;
    }
    
    // start of training, get current word (w)
    word = sen[sentence_position];
    input_word_position = word * embed_max_size;
    if (word == -1) continue;
    next_random = next_random * (unsigned long long)25214903917 + 11;
    b = next_random % window; // Samples(!) window size
    int z_probs_size = embed_current_size + 1;  // lock-in value of embed_current_size for thread since its shared globally
 
    // Get actual valid context
    int pos_context_counter = 0; // size of actual context
    for (a = b; a < window * 2 + 1; a++) if (a != window) {
      c = sentence_position - window + a;
      if (c < 0) continue;
      if (c >= sentence_length) continue;
      last_word = sen[c];
      if (last_word == -1) continue;
      pos_context_store[pos_context_counter] = last_word;
      pos_context_counter++; 
    }

    // these two values together encode p(z|w) 
    float normConst_z_given_w = compute_z_given_w(word, pos_context_store, unnormProbs_z_given_w, pos_context_counter, z_probs_size-1);
       
    // calculate sum necessary for gradient for log(p(z_m|w_i)) with respect to input
    // and calc necessary for gradient for log(p(z_m|w_i)) with respect to context
    for (int i = 0; i < z_probs_size-1; i++) {
      // calculate p(c_v|w_i,z)
      float normConst_c_given_w_z = 0;
      for (int j = 0; j < pos_context_counter; j++) {
        long long pos_context_position = pos_context_store[j] * embed_max_size;
        prob_c_given_w_z[j] = compute_energy(input_word_position, pos_context_position, i+1); 
        normConst_c_given_w_z += prob_c_given_w_z[j];
      }
      // sum_{c_v} [p(c_v|w_i,z) * (c_v * 2w_i)]
      float sum = 0; 
      for (int j = 0; j < pos_context_counter; j++) {
        long long pos_context_position = pos_context_store[j] * embed_max_size;
        sum += (prob_c_given_w_z[j]/normConst_c_given_w_z) * (context_embed[pos_context_position + i] - sparsity_weight*2*input_embed[input_word_position + i]); 
        positive_context_gradient[j * embed_max_size + i] =  (prob_c_given_w_z[j]/normConst_c_given_w_z) * (input_embed[input_word_position + i] - sparsity_weight*2*context_embed[pos_context_position + i]);
      }
      positive_context_fixed_sum_per_dim[i] = sum;
    }
 
    // Taking care of l+1 case for both
    positive_context_fixed_sum_per_dim[z_probs_size - 1] = (dim_penalty / (dim_penalty - 1.0)) * positive_context_fixed_sum_per_dim[z_probs_size - 2];
    for (int j = 0; j < pos_context_counter; j++) {
      positive_context_gradient[j * embed_max_size + z_probs_size-1] = (dim_penalty / (dim_penalty - 1.0)) * positive_context_gradient[j * embed_max_size + z_probs_size-2];
    } 
 
    // iterate through the context words
    float avg_word_acc = 0.0;
    for (a = 0; a < pos_context_counter; a++) {	
        // only need to initialize dimensions less than current_size + 1 since that's all it can grow	
        for (c = 0; c < z_probs_size; c++) {
          input_gradient[c] = 0.0;
          input_gradient_accumulator[c] = 0.0;
          context_gradient_accumulator[c] = 0.0;
          for (d = 0; d < negative; d++) {
            neg_context_gradient_accumulator[d * embed_max_size + c] = 0.0; 
          }
        }
        context_word_position = pos_context_store[a] * embed_max_size;
	        
        if (expand == true) {
          // sample z: z_hat ~ p(z | w) and expand if necessary 
          z_hat = sample_from_mult_list(unnormProbs_z_given_w, z_probs_size, z_vals, num_z_samples, r2);  // no need to normalize, function does it for us 
          //debug_prob(unnormProbs_z_given_w, z_probs_size); 
          //printf("z_hat: %d\n", z_hat);
          if (z_hat == z_probs_size && embed_current_size < z_probs_size && z_hat < embed_max_size) {
	    embed_current_size++; 
	  }
        }
        else {
          z_hat = z_probs_size - 1; //TODO: this should be array of numbers
        }
        
        // NEGATIVE SAMPLING CONTEXT WORDS 
        for (d = 0; d < negative; d++) {
	  next_random = next_random * (unsigned long long)25214903917 + 11;
	  negative_word = table[(next_random >> 16) % table_size];
	  if (negative_word == 0) negative_word = next_random % (vocab_size - 1) + 1;
	  if (negative_word == word) continue; // need to initialize to zeros so this line doesn't fuck up
	  neg_samples[d] = negative_word;
        }

        // CALCULATE GRADIENT FOR EACH SAMPLE 
        for (int k = 0; k < num_z_samples; k++) { 
          float prob_c = 0, Z_c = 0;
          int curr_z = z_vals[k];
          
	  float pos_prob = 0.0; // positive context prob
	  float per_dim_alpha = alpha/num_z_samples; 
          for (d = 0; d < negative; d++) { 
	    negative_word_position = neg_samples[d] * embed_max_size;
	    neg_probs[d] = exp(-compute_energy(input_word_position, negative_word_position, curr_z));
	    Z_c += neg_probs[d];  
	  }
	  // now add the positive example to Z_c
	  pos_prob = exp(-compute_energy(input_word_position, context_word_position, curr_z));
	  Z_c += pos_prob; 

          // iterate through negatives again to compute probabilities and perform updates
	  for (d = 0; d < negative; d++){
	    if (neg_samples[d] > 0){
	      negative_word_position = neg_samples[d] * embed_max_size;
	      prob_c = neg_probs[d]/Z_c;
	      for (c = 0; c < curr_z; c++) {
		// Important!: add to accumulator before updating
		input_gradient[c] += prob_c * (context_embed[negative_word_position + c] - sparsity_weight*2*input_embed[input_word_position + c]);
		neg_context_gradient_accumulator[d * embed_max_size + c] += per_dim_alpha * prob_c * (input_embed[input_word_position + c] - sparsity_weight*2*context_embed[negative_word_position + c]);
	      }
	    }
	  }
          
          prob_c = pos_prob/Z_c;
	  avg_word_acc += prob_c;
          // positive context and input gradient	 
	  for (c = 0; c < curr_z; c++) {
            float input_sum = 0.0, context_sum = 0.0;  
            float input_deriv = context_embed[context_word_position + c] - sparsity_weight*2*input_embed[input_word_position + c];
            float context_deriv = input_embed[input_word_position + c] -  sparsity_weight*2*context_embed[context_word_position + c];

            for (int d = c; d < z_probs_size; d++) { 
	      float is_sampled_z = 0;
              if (d == curr_z - 1) is_sampled_z = 1; 
              input_sum += (is_sampled_z - (unnormProbs_z_given_w[d]/normConst_z_given_w)) * positive_context_fixed_sum_per_dim[d];
	      
              context_sum += (is_sampled_z - (unnormProbs_z_given_w[d]/normConst_z_given_w)) * positive_context_gradient[a * z_probs_size + d]; 
	    }
	    input_gradient[c] += ((prob_c - 1.0) * input_deriv);
            input_gradient_accumulator[c] += per_dim_alpha * ((1.0/(negative+1.0)) * input_gradient[c] + (log(prob_c + epsilon)) * input_sum); 
 
	    context_gradient_accumulator[c] += per_dim_alpha * ((prob_c - 1.0) * context_deriv + (log(prob_c + epsilon)) * context_sum);
          }
  
        }
        
        // apply gradient update to input, positive & negative context
        for (c = 0; c < z_probs_size; c++) {
          input_embed[input_word_position + c] -= input_gradient_accumulator[c];
          context_embed[context_word_position + c] -= context_gradient_accumulator[c]; 
          for (d = 0; d < negative; d++) {
            negative_word_position = neg_samples[d] * embed_max_size;
            context_embed[negative_word_position + c] -= neg_context_gradient_accumulator[d * embed_max_size + c];
          }
        }
    }    
    acc += (avg_word_acc)/(a*num_z_samples);
    sentence_position++; 
    if (sentence_position >= sentence_length) {
      sentence_length = 0;
      continue;
    }
  }
  fclose(fi);
  free(z_vals);   
  free(pos_context_store);
  free(input_gradient_accumulator);
  free(context_gradient_accumulator);
  free(neg_context_gradient_accumulator);
  free(unnormProbs_z_given_w);
  free(positive_context_fixed_sum_per_dim);
  free(positive_context_gradient);
  free(neg_probs);
  free(input_gradient);

  pthread_exit(NULL);
}

void TrainModel() {
  long a;
  pthread_t *pt = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
  printf("Starting training using file %s\n", train_file);
  starting_alpha = alpha;
  if (read_vocab_file[0] != 0) ReadVocab(); else LearnVocabFromTrainFile();
  if (save_vocab_file[0] != 0) SaveVocab();
  if (output_file[0] == 0) return;
  InitNet();
  if (negative > 0) InitUnigramTable();
  start = clock();
 
  int actual_iter = iter;
  if (fixed_dim_iter > 0) {
    // fixed-dim training
    iter = fixed_dim_iter;
    printf("Training fixed dim model for %lld iters \n", iter);
    for (a = 0; a < num_threads; a++) {
      ThreadArg arg;
      arg.id = a;
      arg.expand = false; 
      pthread_create(&pt[a], NULL, TrainModelThread, (void *)&arg);
    }
    for (a = 0; a < num_threads; a++) pthread_join(pt[a], NULL);
    printf("Writing vectors to %s\n", fixed_dim_output_file);
    save_vectors(fixed_dim_output_file, vocab_size, embed_current_size, vocab, input_embed);
  }
  // Reset training info
  iter = actual_iter;
  word_count_actual = 0;
  alpha = starting_alpha;
  // expanded-dim training for desired epochs
  printf("Training expanded dim model for %lld iters \n", iter);
  for (a = 0; a < num_threads; a++) {
    ThreadArg arg;
    arg.id = a;
    arg.expand = true; 
    pthread_create(&pt[a], NULL, TrainModelThread, (void *)&arg);
  }
  for (a = 0; a < num_threads; a++) pthread_join(pt[a], NULL);
  printf("Writing vectors to %s\n", output_file);
  save_vectors(output_file, vocab_size, embed_current_size, vocab, input_embed);
  if (strlen(context_output_file) > 0)  save_vectors(context_output_file, vocab_size, embed_current_size, vocab, context_embed);
}

int ArgPos(char *str, int argc, char **argv) {
  int a;
  for (a = 1; a < argc; a++) if (!strcmp(str, argv[a])) {
      if (a == argc - 1) {
	printf("Argument missing for %s\n", str);
	exit(1);
      }
      return a;
    }
  return -1;
}

// testing function for sampling from multinomial
void multinom_unit_test(){
  // set up random number generator                                                                                                                                                                    
  const gsl_rng_type * T2;
  gsl_rng * r2;
  srand(time(NULL));
  unsigned int Seed2 = rand();
  gsl_rng_env_setup();
  T2 = gsl_rng_default;
  r2 = gsl_rng_alloc (T2);
  gsl_rng_set (r2, Seed2);
  
  double x[] = {0.1, 0.1, 0.1, 0.1, 0.1, 0.1};
  for (int w=0; w<10; w++){
    int y = sample_from_mult(x, 6, r2);
    printf("Sampled idx: %i \n", y);
  }
}

int main(int argc, char **argv) {
  int i;
  if (argc == 1) {
    printf("INFINITE Word Embeddings\n\n");
    printf("Options:\n");
    printf("Parameters for training:\n");
    printf("\t-train <file>\n");
    printf("\t\tUse text data from <file> to train the model\n");
    printf("\t-output <file>\n");
    printf("\t\tUse <file> to save the resulting word vectors\n");
    printf("\t-fixed_dim_output <file> \n");
    printf("\t\tUse <file> to save resulting word vectors of fixed dimension training\n");
    printf("\t-contextOutput <file>\n");
    printf("\t\tUse <file> to save the resulting *context* vectors\n");
    printf("\t-initSize <int>\n");
    printf("\t\tSet the initial dimensionality of the word vectors; default is 5\n");
    printf("\t-maxSize <int>\n");
    printf("\t\tSet the maximum dimensionality of the word vectors; default is 2000\n");
    printf("\t-window <int>\n");
    printf("\t\tSet max skip length between words; default is 5\n");
    printf("\t-sample <float>\n");
    printf("\t\tSet threshold for occurrence of words; Frequent ones will be downsampled.\n");
    printf("\t-negative <int>\n");
    printf("\t\tNumber of negative examples; default is 5, common values are 3 - 10 (0 = not used)\n");
    printf("\t-threads <int>\n");
    printf("\t\tUse <int> threads (default 12)\n");
    printf("\t-iter <int>\n");
    printf("\t\tRun more training iterations (default 5)\n");
    printf("\t-fixed_dim_iter <int>\n");
    printf("\t\tHow many <int> iterations to run fixed-dimension training (default 0)\n");
    printf("\t-min-count <int>\n");
    printf("\t\tThis will discard words that appear less than <int> times; default is 5\n");
    printf("\t-alpha <float>\n");
    printf("\t\tSet the starting learning rate; default is 0.025.\n");
    printf("\t-dimPenalty <int>\n");
    printf("\t\tPenalty incurred for using each embedding dimension.  Must be in (1, infinity) to guarantee convergent Z. default=5.\n");
    printf("\t-sparsityWeight <float>\n");
    printf("\t\tWeight placed on L-2 sparsity penalty.  default = 0.001.\n");
    printf("\t-save-vocab <file>\n");
    printf("\t\tThe vocabulary will be saved to <file>\n");
    printf("\t-read-vocab <file>\n");
    printf("\t\tThe vocabulary will be read from <file>, not constructed from the training data\n");
    printf("\nExamples:\n");
    printf("./iW2V -train data.txt -output w_vec.txt -contextOutput c_vec.txt -initSize 5 -maxSize 2000 -window 5 -sample 1e-4 -negative 5 -iter 3\n\n");
    return 0;
  }
  output_file[0] = 0;
  save_vocab_file[0] = 0;
  read_vocab_file[0] = 0;
  if ((i = ArgPos((char *)"-initSize", argc, argv)) > 0) embed_current_size = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-maxSize", argc, argv)) > 0) embed_max_size = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-train", argc, argv)) > 0) strcpy(train_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-save-vocab", argc, argv)) > 0) strcpy(save_vocab_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-read-vocab", argc, argv)) > 0) strcpy(read_vocab_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-debug", argc, argv)) > 0) debug_mode = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-alpha", argc, argv)) > 0) alpha = atof(argv[i + 1]);
  if ((i = ArgPos((char *)"-dim_penalty", argc, argv)) > 0) dim_penalty = atof(argv[i+1]);
  if ((i = ArgPos((char *)"-sparsityWeight", argc, argv)) > 0) sparsity_weight = atof(argv[i+1]);
  if ((i = ArgPos((char *)"-output", argc, argv)) > 0) strcpy(output_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-contextOutput", argc, argv)) > 0) strcpy(context_output_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-fixed_dim_output", argc, argv)) > 0) strcpy(fixed_dim_output_file, argv[i+1]);
  if ((i = ArgPos((char *)"-window", argc, argv)) > 0) window = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-sample", argc, argv)) > 0) sample = atof(argv[i + 1]);
  if ((i = ArgPos((char *)"-negative", argc, argv)) > 0) negative = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-threads", argc, argv)) > 0) num_threads = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-iter", argc, argv)) > 0) iter = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-fixed_dim_iter", argc, argv)) > 0) fixed_dim_iter = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-min-count", argc, argv)) > 0) min_count = atoi(argv[i + 1]);
  vocab = (struct vocab_word *)calloc(vocab_max_size, sizeof(struct vocab_word));
  vocab_hash = (int *)calloc(vocab_hash_size, sizeof(int));
  print_args();
  TrainModel();
  return 0;
}
      