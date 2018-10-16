#Times of each layer (in ms) with 3 decimal place precision
#TODO: FILL IN

# CONV_L1 = 2394.059
# RELU_L1 = 22.884
# POOL_L1 = 81.863
# CONV_L2 = 998.259
# RELU_L2 = 6.347
# POOL_L2 = 19.392
# CONV_L3 = 256.852
# RELU_L3 = 1.411
# POOL_L3 = 3.342
# FC_L1 = 8.509
# SOFTMAX_L1 = 1.008
# TOTAL_TIME = 3793.926


#old2
# CONV_L1 = 3393.842
# RELU_L1 = 80.353
# POOL_L1 = 248.324
# CONV_L2 = 1352.251
# RELU_L2 = 15.664
# POOL_L2 = 55.850
# CONV_L3 = 350.123
# RELU_L3 = 2.901
# POOL_L3 = 13.701
# FC_L1 = 8.922
# SOFTMAX_L1 = 1.150
# TOTAL_TIME = 5523.081

#old
CONV_L1 = 3420.643
RELU_L1 = 37.688
POOL_L1 = 93.438
CONV_L2 = 3549.732
RELU_L2 = 26.207
POOL_L2 = 29.309
CONV_L3 = 982.790
RELU_L3 = 0.950
POOL_L3 = 7.467
FC_L1 = 6.636
SOFTMAX_L1 = 0.832
TOTAL_TIME = 8155.692


layer_times = [CONV_L1, RELU_L1, POOL_L1, CONV_L2, RELU_L2, POOL_L2, CONV_L3, RELU_L3, POOL_L3, FC_L1, SOFTMAX_L1]
layer_labels = ["CONV_L1", "RELU_L1", "POOL_L1", "CONV_L2", "RELU_L2", "POOL_L2", "CONV_L3", "RELU_L3", "POOL_L3", "FC_L1", "SOFTMAX_L1"]
layer_types = ["CONV", "RELU", "POOL", "FC", "SOFTMAX"]
layer_percents = [x/TOTAL_TIME for x in layer_times]

f = open('answers.txt','w')

f.write("\nQUESTION 1\n\n")
for data in zip(layer_labels, layer_times, layer_percents):
	f.write("{0:<15}: {1:<10} ms / {2:<10.2%}\n".format(*data))

f.write("\nQUESTION 2\n\n")

total_percents = dict(zip(layer_types, [0]*len(layer_types)))

for label, percent in zip(layer_labels, layer_percents):
	total_percents[label[:label.index("_")]] += percent

for label in layer_types:
	f.write("{0:<15}: {1:<10.2%}\n".format(label, total_percents[label]))

f.write("\nQUESTION 3\n\n")

#TODO: FILL IN
ahmdal = lambda p: 1/((1-p)+(p/4))

for label in layer_types:
	f.write("{0:<15}: {1:>3.2f}x\n".format(label, ahmdal(total_percents[label])))

optimal_layer = max(total_percents, key=total_percents.get)

f.write("\nLayer we should optimize: {}\n".format(optimal_layer))
f.close()