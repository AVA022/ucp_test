#include <cstdlib>
#include <iostream>
#include <string.h>

using namespace std;

static int * vector_buffer;
static int ** chunk_ptrs;

static const int VECTOR_SIZE = 64;
static const int NUM_CHUNK = 2;

static bool init_chunk_ptrs()
{
    chunk_ptrs = (int **)malloc(NUM_CHUNK * sizeof(int *));
    if (chunk_ptrs == NULL) {
        return false;
    }

    for (int i = 0; i < NUM_CHUNK; i++) {
        chunk_ptrs[i] = vector_buffer + i * VECTOR_SIZE / NUM_CHUNK;
    }

    return true;
}

static void free_chunk_ptrs()
{
    free(chunk_ptrs);
}

static int init_vector_buffer()
{
    vector_buffer = (int *)malloc(VECTOR_SIZE * sizeof(int));
    if (vector_buffer == NULL) {
        return -1;
    }

    for (int i = 0; i < VECTOR_SIZE; i++) {
        vector_buffer[i] = i;
    }

    return 0;
}

static void free_vector_buffer()
{
    free(vector_buffer);
}

static bool check_result_vector_buffer()
{
    for (int i = 0; i < VECTOR_SIZE; i++) {
        if (vector_buffer[i] != 2 * i) {
            return false;
        }
    }

    return true;
}

int main(){
    init_vector_buffer();
    init_chunk_ptrs();

    //memcpy(chunk_ptrs[1], chunk_ptrs[0], VECTOR_SIZE / NUM_CHUNK * sizeof(int)); // mem

    for(int i = 0; i < VECTOR_SIZE / NUM_CHUNK; i ++)
    {
        cout << chunk_ptrs[1][i] << endl;
    }

    free_chunk_ptrs();
    free_vector_buffer();
    return 0;
}

