import multiprocessing.shared_memory as shm
import time
from multiprocessing.resource_tracker import unregister
import atexit
from matplotlib import pyplot as plt
import numpy as np
import sys

w = 64
h = 64
_size = w*h*3
_shape = (h, w, 3)

def main():
    shared_memory = shm.SharedMemory(name='melee_shm_frame', create=False, size=_size)

    while True:
        data = shared_memory.buf
        data_np = np.frombuffer(data, dtype=np.uint8)
        #data_np = data_np[:_size]
        reshaped = data_np.reshape(_shape)
        plt.imshow(reshaped, interpolation='nearest')
        fig = plt.show()
        print('first = ' + str(data[0]))
        plt.close(fig)
    

if __name__=='__main__':
    main()