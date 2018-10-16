#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include "timestamp.c"

// Include SSE intrinsics
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#include <x86intrin.h>
#endif

// Include OpenMP
#include <omp.h>

// Vol ------------------------------------------------------------------------

// Volumes are used to represent the activations (i.e., state) between the
// different layers of the CNN. They all have three dimensions. The inter-
// pretation of their content depends on the layer that produced them. Before
// the first iteration, the Volume holds the data of the image we want to
// classify (the depth are the three color dimensions). After the last stage
// of the CNN, the Volume holds the probabilities that an image is part of
// a specific category.

/*
 * Represents a three-dimensional array of numbers, and its size. The numbers
 * at (x,y,d) are stored in array w at location ((v->sx * y)+x)*v->depth+d.
 */

typedef struct vol {
    uint64_t sx,sy,depth;
    double* w;
} vol_t;

/*
 * Set the value at a specific entry of the array.
 */

static inline double get_vol(vol_t* v, int x, int y, int d) {
    return v->w[((v->sx * y)+x)*v->depth+d];
}

/*
 * Get the value at a specific entry of the array.
 */

static inline void set_vol(vol_t* v, int x, int y, int d, double val) {
    v->w[((v->sx * y)+x)*v->depth+d] = val;
}

/*
 * Allocate a new array with specific dimensions and default value v.
 */

static vol_t* make_vol(int sx, int sy, int d, double v) {
    vol_t* out = (vol_t*)malloc(sizeof(struct vol));
    out->w = (double*)malloc(sizeof(double)*(sx*sy*d));
    out->sx = sx;
    out->sy = sy;
    out->depth = d;
#pragma omp parallel
    {
#pragma omp for
        for (int x = 0; x < sx; x++)
            for (int y = 0; y < sy; y++)
                for (int z = 0; z < d; z++)
                    set_vol(out, x, y, z, v);
    }
    return out;
}

/*
 * Copy the contents of one Volume to another (assuming same dimensions).
 */

static vol_t* copy_vol(vol_t* dest, vol_t* src) {
    #pragma omp parallel
    {
    #pragma omp for 
    for (int x = 0; x < dest->sx; x++)
        for (int y = 0; y < dest->sy; y++)
            for (int z = 0; z < dest->depth; z++)
                set_vol(dest, x, y, z, get_vol(src, x, y, z));
    }
}

/*
 * Deallocate the array.
 */
void free_vol(vol_t* v) {
    free(v->w);
    free(v);
}

// A note about layers --------------------------------------------------------

/*
 * What follows are the different layers of the CNN. You will not have to
 * understand what these layers are actually doing. In general terms, each
 * layer performs a "forward" operation on a batch of inputs. During this
 * forward operation, the layer takes a set of input Volumes and transforms
 * them into a set of output Volumes (one output for each input). What differs
 * is the operation performed by each layer.
 *
 * In addition to the _forward function, each layer also provides a data
 * structure, holding (fixed) parameters for that layer, a make_ function to
 * allocate an instance of the layer with a particular set of parameters and
 * a load function to load training data for that layer from a file. Note that
 * you will not have to make any changes to any of these functions. The only
 * function you need to consider is the _forward function.
 */

// Convolutional Layer --------------------------------------------------------

typedef struct conv_layer {
    // required
    int out_depth;
    int sx;
    int in_depth;
    int in_sx;
    int in_sy;
    
    // optional
    int sy;
    int stride;
    int pad;
    double l1_decay_mul;
    double l2_decay_mul;
    
    // computed
    int out_sx;
    int out_sy;
    double bias;
    vol_t* biases;
    vol_t** filters;
} conv_layer_t;

conv_layer_t* make_conv_layer(int in_sx, int in_sy, int in_depth,
                              int sx, int filters, int stride, int pad) {
    conv_layer_t* l = (conv_layer_t*)malloc(sizeof(conv_layer_t));
    // required
    l->out_depth = filters;
    l->sx = sx;
    l->in_depth = in_depth;
    l->in_sx = in_sx;
    l->in_sy = in_sy;
    
    // optional
    l->sy = l->sx;
    l->stride = stride;
    l->pad = pad;
    l->l1_decay_mul = 0.0;
    l->l2_decay_mul = 1.0;
    
    // computed
    l->out_sx = floor((l->in_sx + l->pad * 2 - l->sx) / l->stride + 1);
    l->out_sy = floor((l->in_sy + l->pad * 2 - l->sy) / l->stride + 1);
    
    l->filters = (vol_t**)malloc(sizeof(vol_t*)*filters);
    for (int i = 0; i < filters; i++) {
        l->filters[i] = make_vol(l->sx, l->sy, l->in_depth, 0.0);
    }
    
    l->bias = 0.0;
    l->biases = make_vol(1, 1, l->out_depth, l->bias);
    
    return l;
}

//depth == 3
void conv_forward_1(conv_layer_t* l, vol_t** in, vol_t** out) {
    double* V_w = in[0]->w;
    double* A_w = out[0]->w;
    for(int d = 0; d < 16; d++) {
        double l_w = l->biases->w[d];
        vol_t* f = l->filters[d];
        int y = -2;
        double *f_w = f->w;
        for(int ay = 0; ay < 32; ay++) {
            int x = -2;
            int a_y = ay *512;
            for(int ax=0; ax < 32; ax++) {
                double a = 0.0;
                for(int fx = 0; fx < 5; fx++) {
                    int ox = x + fx;
                    if(ox < 32 && ox > -1) {
                        for(int fy = 0; fy < 5; fy++) {
                            int oy = y + fy;
                            if(oy < 32 && oy > -1){
                                double* f_addr = f_w + (5 * fy + fx) * 3;
                                double* V_addr = V_w + (32 * oy + ox) * 3;
                                a += *(f_addr) * *(V_addr);
                                a += *(f_addr+1) * *(V_addr+1);
                                a += *(f_addr+2) * *(V_addr+2);
                            }
                        }
                    }
                }
                *(A_w + (a_y) + (ax * 16) + d) = a + l_w;
                x += 1;
            }
            y += 1;
        }
    }
}

  
  
//depth == 16

void conv_forward_2(conv_layer_t* l, vol_t** in, vol_t** out) {
    double result64_[4];
    double* V_w = in[0]->w;
    double* A_w = out[0]->w;

    for(int d = 0; d < 20; d++) {
        vol_t* f = l->filters[d];
        double* f_w = f->w;
        double l_w = l->biases->w[d];
        int y = -2;
        for(int ay = 0; ay < 16;y += 1, ay++) {
            int x = -2;
            int a_y = 16*ay;
            for(int ax=0; ax < 16; x += 1, ax++) {
                double a = 0.0;
                __m256d sum = _mm256_setzero_pd();

                for(int fy = 0; fy < 5; fy++) {
                    int oy = y + fy;
                    if(oy > -1 && oy < 16 ){
                        for(int fx = 0; fx < 5; fx++) {
                            int ox = x + fx;
                            if(ox > -1 && ox < 16) {
                                double* f_addr = f_w + (5 * fy+fx)*16;
                                double* V_addr = V_w + (16 * oy+ox)*16;
                                
                                sum = _mm256_add_pd(sum, _mm256_mul_pd( _mm256_loadu_pd (f_addr), _mm256_loadu_pd (V_addr)));
                                sum = _mm256_add_pd(sum, _mm256_mul_pd( _mm256_loadu_pd (f_addr+4), _mm256_loadu_pd (V_addr+4)));
                                sum = _mm256_add_pd(sum, _mm256_mul_pd( _mm256_loadu_pd (f_addr+8), _mm256_loadu_pd (V_addr+8)));
                                sum = _mm256_add_pd(sum, _mm256_mul_pd( _mm256_loadu_pd (f_addr+12), _mm256_loadu_pd (V_addr+12)));
                            }
                        }
                    }
                }
                _mm256_store_pd(result64_, sum);
                *(A_w + (a_y + ax)* 20 + d) = a + result64_[0] + result64_[1] + result64_[2] + result64_[3] + l_w;
            }
        }
    }
}


      

//depth == 20
void conv_forward_3(conv_layer_t* l, vol_t** in, vol_t** out) {
    double result64_[4];
        
    double* V_w = in[0]->w;
    double* A_w = out[0]->w;
    for(int d = 0; d < 20; d++) {
        vol_t* f = l->filters[d];
        double* f_w = f->w;
        double l_w = l->biases->w[d];
        int y = -2;
        for(int ay = 0; ay < 8; y += 1, ay++) {
            int x = -2;
            int a_y = 8 * ay;
            for(int ax=0; ax < 8; x += 1, ax++) {
                double a = 0.0;
                __m256d sum = _mm256_setzero_pd();
                for(int fy = 0; fy < 5; fy++) {
                    int oy = y + fy;
                    if(oy > -1 && oy < 8 ){
                        for(int fx = 0; fx < 5; fx++) {
                            int ox = x + fx;
                            if(ox > -1 && ox < 8) {
                                double* f_addr = f_w + (5 * fy+fx)*20;
                                double* V_addr = V_w + (8 * oy+ox)*20;
                            
                                sum = _mm256_add_pd(sum, _mm256_mul_pd( _mm256_loadu_pd (f_addr), _mm256_loadu_pd (V_addr)));
                                sum = _mm256_add_pd(sum, _mm256_mul_pd( _mm256_loadu_pd (f_addr+4), _mm256_loadu_pd (V_addr+4)));
                                sum = _mm256_add_pd(sum, _mm256_mul_pd( _mm256_loadu_pd (f_addr+8), _mm256_loadu_pd (V_addr+8)));
                                sum = _mm256_add_pd(sum, _mm256_mul_pd( _mm256_loadu_pd (f_addr+12), _mm256_loadu_pd (V_addr+12)));
                                sum = _mm256_add_pd(sum, _mm256_mul_pd( _mm256_loadu_pd (f_addr+16), _mm256_loadu_pd (V_addr+16)));
                            }
                        }
                    }
                }
                _mm256_store_pd(result64_, sum);
                
                *(A_w + (a_y + ax) * 20 + d) = a + result64_[0] + result64_[1] + result64_[2] + result64_[3] + l_w;
            }
        }
    }
}




void conv_load(conv_layer_t* l, const char* fn) {
    int sx, sy, depth, filters;
    
    FILE* fin = fopen(fn, "r");
    
    fscanf(fin, "%d %d %d %d", &sx, &sy, &depth, &filters);
    assert(sx == l->sx);
    assert(sy == l->sy);
    assert(depth == l->in_depth);
    assert(filters == l->out_depth);
    
    for(int d = 0; d < l->out_depth; d++)
        for (int x = 0; x < sx; x++)
            for (int y = 0; y < sy; y++)
                for (int z = 0; z < depth; z++) {
                    double val;
                    fscanf(fin, "%lf", &val);
                    set_vol(l->filters[d], x, y, z, val);
                }
    
    for(int d = 0; d < l->out_depth; d++) {
        double val;
        fscanf(fin, "%lf", &val);
        set_vol(l->biases, 0, 0, d, val);
    }
    
    fclose(fin);
}

// Relu Layer -----------------------------------------------------------------

typedef struct relu_layer {
    // required
    int in_depth;
    int in_sx;
    int in_sy;
    
    // computed
    int out_depth;
    int out_sx;
    int out_sy;
} relu_layer_t;

relu_layer_t* make_relu_layer(int in_sx, int in_sy, int in_depth) {
    relu_layer_t* l = (relu_layer_t*)malloc(sizeof(relu_layer_t));
    
    // required
    l->in_depth = in_depth;
    l->in_sx = in_sx;
    l->in_sy = in_sy;
    
    // computed
    l->out_sx = l->in_sx;
    l->out_sy = l->in_sy;
    l->out_depth = l->in_depth;
    
    return l;
}

void relu_forward_1(relu_layer_t* l, vol_t** in, vol_t** out) {
    //for (int j = start; j <= end; j++) {
        for (int i = 0; i < 16384; i++) {
            out[0]->w[i] = (in[0]->w[i] < 0.0) ? 0.0 : in[0]->w[i];
        }
    //}
}


void relu_forward_2(relu_layer_t* l, vol_t** in, vol_t** out) {
    //for (int j = start; j <= end; j++) {
        for (int i = 0; i < 5120; i++) {
            out[0]->w[i] = (in[0]->w[i] < 0.0) ? 0.0 : in[0]->w[i];
        }
    //}
}

void relu_forward_3(relu_layer_t* l, vol_t** in, vol_t** out) {
    //for (int j = start; j <= end; j++) {
        for (int i = 0; i < 1280; i++) {
            out[0]->w[i] = (in[0]->w[i] < 0.0) ? 0.0 : in[0]->w[i];
        }
    //}
}

// Pool Layer -----------------------------------------------------------------

typedef struct pool_layer {
    // required
    int sx;
    int in_depth;
    int in_sx;
    int in_sy;
    
    // optional
    int sy;
    int stride;
    int pad;
    
    // computed
    int out_depth;
    int out_sx;
    int out_sy;
} pool_layer_t;

pool_layer_t* make_pool_layer(int in_sx, int in_sy, int in_depth,
                              int sx, int stride) {
    pool_layer_t* l = (pool_layer_t*)malloc(sizeof(pool_layer_t));
    
    // required
    l->sx = sx;
    l->in_depth = in_depth;
    l->in_sx = in_sx;
    l->in_sy = in_sy;
    
    // optional
    l->sy = l->sx;
    l->stride = stride;
    l->pad = 0;
    
    // computed
    l->out_depth = in_depth;
    l->out_sx = floor((l->in_sx + l->pad * 2 - l->sx) / l->stride + 1);
    l->out_sy = floor((l->in_sy + l->pad * 2 - l->sy) / l->stride + 1);
    
    return l;
}

void pool_forward_1(pool_layer_t* l, vol_t** in, vol_t** out) {
    //for (int i = start; i <= end; i++) {
        vol_t* V = in[0];
        vol_t* A = out[0];
        double* V_w = V->w;
        
        int n=0;
        for(int d=0;d<16;d++) {
            int x = 0;
            for(int ax=0; ax<16; x+=2,ax++) {
                int y = 0;
                for(int ay=0; ay<16; y+=2,ay++) {
                    double a = -99999;
                    for(int fx=0;fx<2;fx++) {
                        int ox = x+fx;
                        if(ox>=0 && ox<32){
                            for(int fy=0;fy<2;fy++) {
                                int oy = y+fy;
                                if(oy>=0 && oy<32) {
                                    double v = *(V_w + ((V->sx * oy)+ox)*16+d);
                                    if(v > a) { a = v; }
                                }
                            }
                        }
                    }
                    n++;
                    set_vol(A, ax, ay, d, a);
                }
            }
        }
    //}
}


void pool_forward_2(pool_layer_t* l, vol_t** in, vol_t** out) {
    //for (int i = start; i <= end; i++) {
        vol_t* V = in[0];
        vol_t* A = out[0];
        double* V_w = V->w;

        int n=0;
        for(int d=0;d<20;d++) {
            int x = 0;
            for(int ax=0; ax<8; x+=2,ax++) {
                int y = 0;
                for(int ay=0; ay<8; y+=2,ay++) {
                    double a = -99999;
                    for(int fx=0;fx<2;fx++) {
                        int ox = x+fx;
                        if(ox>=0 && ox<16){
                            for(int fy=0;fy<2;fy++) {
                                int oy = y+fy;
                                if(oy>=0 && oy<16) {
                                    
                                    double v = *(V_w + ((V->sx * oy)+ox)*20+d);
                                    if(v > a) { a = v; }
                                }
                            }
                        }
                    }
                    n++;
                    set_vol(A, ax, ay, d, a);
                }
            }
        }
    //}
}


void pool_forward_3(pool_layer_t* l, vol_t** in, vol_t** out) {
    //for (int i = start; i <= end; i++) {
        vol_t* V = in[0];
        vol_t* A = out[0];
        double* V_w = V->w;
        
        int n=0;
        for(int d=0;d<20;d++) {
            int x = 0;
            for(int ax=0; ax<4; x+=2,ax++) {
                int y = 0;
                for(int ay=0; ay<4; y+=2,ay++) {
                    double a = -99999;
                    for(int fx=0;fx<2;fx++) {
                        int ox = x+fx;
                        if(ox>=0 && ox<8){
                            for(int fy=0;fy<2;fy++) {
                                int oy = y+fy;
                                if(oy>=0 && oy<8) {
                                    double v = *(V_w + ((V->sx * oy)+ox)*20+d);
                                    if(v > a) { a = v; }
                                }
                            }
                        }
                    }
                    n++;
                    set_vol(A, ax, ay, d, a);
                }
            }
        }
    //}
}


// FC Layer -------------------------------------------------------------------

typedef struct fc_layer {
    // required
    int out_depth;
    int in_depth;
    int in_sx;
    int in_sy;
    
    // optional
    double l1_decay_mul;
    double l2_decay_mul;
    
    // computed
    int out_sx;
    int out_sy;
    int num_inputs;
    double bias;
    vol_t* biases;
    vol_t** filters;
} fc_layer_t;

fc_layer_t* make_fc_layer(int in_sx, int in_sy, int in_depth,
                          int num_neurons) {
    fc_layer_t* l = (fc_layer_t*)malloc(sizeof(fc_layer_t));
    
    // required
    l->out_depth = num_neurons;
    l->in_depth = in_depth;
    l->in_sx = in_sx;
    l->in_sy = in_sy;
    
    // optional
    l->l1_decay_mul = 0.0;
    l->l2_decay_mul = 1.0;
    
    // computed
    l->num_inputs = l->in_sx * l->in_sy * l->in_depth;
    l->out_sx = 1;
    l->out_sy = 1;
    
    l->filters = (vol_t**)malloc(sizeof(vol_t*)*num_neurons);
    for (int i = 0; i < l->out_depth; i++) {
        l->filters[i] = make_vol(1, 1, l->num_inputs, 0.0);
    }
    
    l->bias = 0.0;
    l->biases = make_vol(1, 1, l->out_depth, l->bias);
    
    return l;
}

void fc_forward(fc_layer_t* l, vol_t** in, vol_t** out) {
    //for (int j = start; j <= end; j++) {
        vol_t* V = in[0];
        vol_t* A = out[0];
        double* V_w = V->w;

        for(int i=0;i<10;i++) {
            double a = 0.0;
            for(int d=0;d<320;d++) {
                a += *(V_w + d) * l->filters[i]->w[d];
            }
            a += l->biases->w[i];
            A->w[i] = a;
        }
    //}
}

void fc_load(fc_layer_t* l, const char* fn) {
    FILE* fin = fopen(fn, "r");
    int num_inputs;
    int out_depth;
    fscanf(fin, "%d %d", &num_inputs, &out_depth);
    assert(out_depth == l->out_depth);
    assert(num_inputs == l->num_inputs);
    
    for(int i = 0; i < l->out_depth; i++)
        for(int d = 0; d < l->num_inputs; d++) {
            double val;
            fscanf(fin, "%lf", &val);
            l->filters[i]->w[d] = val;
        }
    
    for(int i = 0; i < l->out_depth; i++) {
        double val;
        fscanf(fin, "%lf", &val);
        l->biases->w[i] = val;
    }
    
    fclose(fin);
}

// Softmax Layer --------------------------------------------------------------

// Maximum supported out_depth
#define MAX_ES 16

typedef struct softmax_layer {
    // required
    int in_depth;
    int in_sx;
    int in_sy;
    double* es;
    
    // computed
    int out_depth;
    int out_sx;
    int out_sy;
} softmax_layer_t;

softmax_layer_t* make_softmax_layer(int in_sx, int in_sy, int in_depth) {
    softmax_layer_t* l = (softmax_layer_t*)malloc(sizeof(softmax_layer_t));
    
    // required
    l->in_depth = in_depth;
    l->in_sx = in_sx;
    l->in_sy = in_sy;
    
    // computed
    l->out_sx = 1;
    l->out_sy = 1;
    l->out_depth = l->in_sx * l->in_sy * l->in_depth;
    
    l->es = (double*)malloc(sizeof(double)*l->out_depth);
    
    return l;
}

void softmax_forward(softmax_layer_t* l, vol_t** in, vol_t** out) {
    double es[MAX_ES];
    
    //for (int j = start; j <= end; j++) {
        vol_t* V = in[0];
        vol_t* A = out[0];
        // compute max activation
        double amax = V->w[0];
        for(int i=1;i<10;i++) {
            if(V->w[i] > amax) amax = V->w[i];
        }
        // compute exponentials (carefully to not blow up)
        double esum = 0.0;
        for(int i=0;i<10;i++) {
            double e = exp(V->w[i] - amax);
            esum += e;
            es[i] = e;
        }
        // normalize and output to sum to one
        for(int i=0;i<10;i++) {
            es[i] /= esum;
            A->w[i] = es[i];
        }
    //}
}

// Neural Network -------------------------------------------------------------

/*
 * This represents the CNN we will use in this project. It consists of 11
 * layers, which means that there are 12 volumes of data (where the first one
 * is the input data and the last one the classification result).
 */

#define LAYERS 11

typedef struct network {
    vol_t* v[LAYERS+1];
    conv_layer_t* l0;
    relu_layer_t* l1;
    pool_layer_t* l2;
    conv_layer_t* l3;
    relu_layer_t* l4;
    pool_layer_t* l5;
    conv_layer_t* l6;
    relu_layer_t* l7;
    pool_layer_t* l8;
    fc_layer_t* l9;
    softmax_layer_t* l10;
} network_t;

/*
 * Instantiate our specific CNN.
 */

network_t* make_network() {
    network_t* net = (network_t*)malloc(sizeof(network_t));
    net->v[0] = make_vol(32, 32, 3, 0.0);
    net->l0 = make_conv_layer(32, 32, 3, 5, 16, 1, 2);
    net->v[1] = make_vol(net->l0->out_sx, net->l0->out_sy, net->l0->out_depth, 0.0);
    net->l1 = make_relu_layer(net->v[1]->sx, net->v[1]->sy, net->v[1]->depth);
    net->v[2] = make_vol(net->l1->out_sx, net->l1->out_sy, net->l1->out_depth, 0.0);
    net->l2 = make_pool_layer(net->v[2]->sx, net->v[2]->sy, net->v[2]->depth, 2, 2);
    net->v[3] = make_vol(net->l2->out_sx, net->l2->out_sy, net->l2->out_depth, 0.0);
    net->l3 = make_conv_layer(net->v[3]->sx, net->v[3]->sy, net->v[3]->depth, 5, 20, 1, 2);
    net->v[4] = make_vol(net->l3->out_sx, net->l3->out_sy, net->l3->out_depth, 0.0);
    net->l4 = make_relu_layer(net->v[4]->sx, net->v[4]->sy, net->v[4]->depth);
    net->v[5] = make_vol(net->l4->out_sx, net->l4->out_sy, net->l4->out_depth, 0.0);
    net->l5 = make_pool_layer(net->v[5]->sx, net->v[5]->sy, net->v[5]->depth, 2, 2);
    net->v[6] = make_vol(net->l5->out_sx, net->l5->out_sy, net->l5->out_depth, 0.0);
    net->l6 = make_conv_layer(net->v[6]->sx, net->v[6]->sy, net->v[6]->depth, 5, 20, 1, 2);
    net->v[7] = make_vol(net->l6->out_sx, net->l6->out_sy, net->l6->out_depth, 0.0);
    net->l7 = make_relu_layer(net->v[7]->sx, net->v[7]->sy, net->v[7]->depth);
    net->v[8] = make_vol(net->l7->out_sx, net->l7->out_sy, net->l7->out_depth, 0.0);
    net->l8 = make_pool_layer(net->v[8]->sx, net->v[8]->sy, net->v[8]->depth, 2, 2);
    net->v[9] = make_vol(net->l8->out_sx, net->l8->out_sy, net->l8->out_depth, 0.0);
    net->l9 = make_fc_layer(net->v[9]->sx, net->v[9]->sy, net->v[9]->depth, 10);
    net->v[10] = make_vol(net->l9->out_sx, net->l9->out_sy, net->l9->out_depth, 0.0);
    net->l10 = make_softmax_layer(net->v[10]->sx, net->v[10]->sy, net->v[10]->depth);
    net->v[11] = make_vol(net->l10->out_sx, net->l10->out_sy, net->l10->out_depth, 0.0);
    return net;
}

/*
 * Free our specific CNN.
 */

void free_network(network_t* net) {
    for (int i = 0; i < LAYERS+1; i++)
        free_vol(net->v[i]);
    
    free(net->l0);
    free(net->l1);
    free(net->l2);
    free(net->l3);
    free(net->l4);
    free(net->l5);
    free(net->l6);
    free(net->l7);
    free(net->l8);
    free(net->l9);
    free(net->l10);
    
    free(net);
}

/*
 * We organize data as "batches" of volumes. Each batch consists of a number of samples,
 * each of which contains a volume for every intermediate layer. Say we have L layers
 * and a set of N input images. Then batch[l][n] contains the volume at layer l for
 * input image n.
 *
 * By using batches, we can process multiple images at once in each run of the forward
 * functions of the different layers.
 */

typedef vol_t** batch_t;

/*
 * This function allocates a new batch for the network old_net with size images.
 */

batch_t* make_batch(network_t* old_net, int size) {
    batch_t* out = (batch_t*)malloc(sizeof(vol_t**)*(LAYERS+1));
    for (int i = 0; i < LAYERS+1; i++) {
        out[i] = (vol_t**)malloc(sizeof(vol_t*)*size);
        for (int j = 0; j < size; j++) {
            out[i][j] = make_vol(old_net->v[i]->sx, old_net->v[i]->sy, old_net->v[i]->depth, 0.0);
        }
    }
    
    return out;
}

/*
 * Free a previously allocated batch.
 */

void free_batch(batch_t* v, int size) {
    for (int i = 0; i < LAYERS+1; i++) {
        for (int j = 0; j < size; j++) {
            free_vol(v[i][j]);
        }
        free(v[i]);
    }
    free(v);
}

/*
 * Apply our network to a specific batch of inputs. The batch has to be given
 * as input to v and start/end are the first and the last image in that batch
 * to process (start and end are inclusive).
 */


uint64_t CONV_L1 = 0;
uint64_t RELU_L1 = 0;
uint64_t POOL_L1 = 0;
uint64_t CONV_L2 = 0;
uint64_t RELU_L2 = 0;
uint64_t POOL_L2 = 0;
uint64_t CONV_L3 = 0;
uint64_t RELU_L3 = 0;
uint64_t POOL_L3 = 0;
uint64_t FC_L1 = 0;
uint64_t SOFTMAX_L1 = 0;
uint64_t TOTAL_TIME = 0;

void net_forward(network_t* net, batch_t* v, int start, int end) {
    //uint64_t time_start = 0, time_end = 0;

    //time_start = timestamp_us();
    conv_forward_1(net->l0, v[0], v[1]);
    // time_end = timestamp_us();
    // CONV_L1 += (time_end - time_start);

    // time_start = timestamp_us();
    relu_forward_1(net->l1, v[1], v[2] );
    // time_end = timestamp_us();
    // RELU_L1 += (time_end - time_start);
    
    // time_start = timestamp_us();
    pool_forward_1(net->l2, v[2], v[3] );
    // time_end = timestamp_us();
    // POOL_L1 += (time_end - time_start);

    // time_start = timestamp_us(); 
    conv_forward_2(net->l3, v[3], v[4]);
    // time_end = timestamp_us();
    // CONV_L2 += (time_end - time_start);

    // time_start = timestamp_us();
    relu_forward_2(net->l4, v[4], v[5] );
    // time_end = timestamp_us();
    // RELU_L2 += (time_end - time_start);
    
    // time_start = timestamp_us();
    pool_forward_2(net->l5, v[5], v[6] );
    // time_end = timestamp_us();
    // POOL_L2 += (time_end - time_start);
    
    // time_start = timestamp_us();
    conv_forward_3(net->l6, v[6], v[7]);
    // time_end = timestamp_us();
    // CONV_L3 += (time_end - time_start);
    
    // time_start = timestamp_us();
    relu_forward_3(net->l7, v[7], v[8] );
    // time_end = timestamp_us();
    // RELU_L3 += (time_end - time_start);
    
    // time_start = timestamp_us();
    pool_forward_3(net->l8, v[8], v[9] );
    // time_end = timestamp_us();
    // POOL_L3 += (time_end - time_start);
    
    // time_start = timestamp_us();
    fc_forward(net->l9, v[9], v[10] );
    // time_end = timestamp_us();
    // FC_L1 += (time_end - time_start);
    
    // time_start = timestamp_us();
    softmax_forward(net->l10, v[10], v[11]);
    // time_end = timestamp_us();
    // SOFTMAX_L1 += (time_end - time_start);
}

/*
 * Putting everything together: Take a set of n input images as 3-dimensional
 * Volumes and process them using the CNN in batches of 1. Then look at the
 * output (which is a set of 10 labels, each of which tells us the likelihood
 * of a specific category) and classify the image as a cat iff the likelihood
 * of "cat" is larger than 50%. Writes the cat likelihood for all images into
 * an output array (0 = definitely no cat, 1 = definitely cat).
 */

#define CAT_LABEL 3
void net_classify_cats(network_t* net, vol_t** input, double* output, int n) {
    #pragma omp parallel
    {
        batch_t* batch = make_batch(net, 1);
        #pragma omp for simd
        for (int i = 0; i < n; i++) {
            copy_vol(batch[0][0], input[i]);
            net_forward(net, batch, 0 ,0 );
            output[i] = batch[11][0]->w[3];
        }
        
        free_batch(batch, 1);
    }
    // TOTAL_TIME = CONV_L1+CONV_L2+CONV_L3+
    // RELU_L1+RELU_L2+RELU_L3+
    // POOL_L1+POOL_L2+POOL_L3+
    // FC_L1+SOFTMAX_L1;
    // printf("CONV_L1 = %lu\n", CONV_L1);
    // printf("RELU_L1 = %lu\n", RELU_L1);
    // printf("POOL_L1 = %lu\n", POOL_L1);
    // printf("CONV_L2 = %lu\n", CONV_L2);
    // printf("RELU_L2 = %lu\n", RELU_L2);
    // printf("POOL_L2 = %lu\n", POOL_L2);
    // printf("CONV_L3 = %lu\n", CONV_L3);
    // printf("RELU_L3 = %lu\n", RELU_L3);
    // printf("POOL_L3 = %lu\n", POOL_L3);
    // printf("FC_L1 = %lu\n", FC_L1);
    // printf("SOFTMAX_L1 = %lu\n", SOFTMAX_L1);
    // printf("TOTAL_TIME = %lu\n", TOTAL_TIME);
}

// IGNORE EVERYTHING BELOW THIS POINT -----------------------------------------

// Including C files in other C files is very bad style and should be avoided
// in any real application. We do it here since we want everything that you
// may edit to be in one file, without having to fix the interfaces between
// the different components of the system.

#include "util.c"
#include "main.c"
