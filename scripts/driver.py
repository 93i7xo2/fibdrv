import subprocess
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
from scipy import stats
import os
from tqdm import tqdm

runs = 100
offset = 100
col = list(range(0, offset))
idx = list(range(0, runs))

def filter(df):
    # df (DataFrame):
    #      ---- offset -----
    # run |     [data]
    #
    # ret (Series):
    #       ---- offset -----
    # mean |     [data]
    ret = np.zeros(df.shape[1])
    t = stats.t(df=(runs-1)).ppf((0.025, 0.975))
    upper_bound = df.mean(0) + t[1]*df.std(0)/(runs**0.5)
    lower_bound = df.mean(0) + t[0]*df.std(0)/(runs**0.5)
    for column in df.columns:
        s = df[column]
        s = s[s <= upper_bound[column]]
        s = s[s >= lower_bound[column]]
        ret[column] = s.mean()
    return ret


def test(mode):
    target_cpu = os.cpu_count()-1
    # time_df:
    #      ---- offset -----
    # run |     [data]
    ktime_df = pd.DataFrame(np.zeros((runs, offset)),
                            index=idx,
                            columns=col)
    utime_df = pd.DataFrame(np.zeros((runs, offset)),
                            index=idx,
                            columns=col)

    subprocess.run(
            f'sudo /bin/sh -c "echo {mode} > /sys/kernel/fibdrv/mode"', shell=True)

    for i in tqdm(range(runs)):
        ret = subprocess.run(
            f'sudo taskset -c {target_cpu} ./client > /dev/null', shell=True)
        df = pd.read_csv('data.txt', delimiter=' ', header=None)
        df.columns = ['idx', 'utime', 'ktime']
        utime_df.loc[i] = df.utime[:offset]
        ktime_df.loc[i] = df.ktime[:offset]

    # result:
    #          user | kernel | kernel to user
    # offset |           [data]
    result = pd.DataFrame({
        'user': filter(utime_df),
        'kernel': filter(ktime_df),
        'kernel to user': filter(utime_df - ktime_df),
    }, index=col)
    return result

if __name__ == "__main__":
    original = test(0)
    fast_doubling = test(1)

    result = pd.DataFrame({
        'original': original['user'],
        'fast doubling': fast_doubling['user'],
    }, index=col)

    ax = result.plot(xlabel='n-th fibonacci', ylabel='time (ns)', title='runtime')
    plt.legend(loc='upper right')
    plt.savefig("runtime.png")
