#include <mpi.h>
#include <iostream>
#include <vector>

void warmup(MPI_Comm comm, int max_size) {
    std::vector<int> buf(max_size, 1);  // 使用 int 类型初始化
    for (int n = 1024; n <= max_size; n *= 2) {
        MPI_Allreduce(MPI_IN_PLACE, buf.data(), n, MPI_INT, MPI_SUM, comm);  // 使用 MPI_INT
    }
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    //预热网络
    warmup(MPI_COMM_WORLD, 1048576);

    //主测试循环
    for (int n = 256; n <= 1048576; n *= 2) {
        std::vector<int> sendbuf(n, rank);  // 使用 int 类型
        std::vector<int> recvbuf(n, 0);     // 使用 int 类型

        double start = MPI_Wtime();
        MPI_Allreduce(sendbuf.data(), recvbuf.data(), n, MPI_INT, MPI_SUM, MPI_COMM_WORLD);  // 使用 MPI_INT
        double end = MPI_Wtime();

        if (rank == 0) {
            double duration = end - start;
            double dataSizeBytes = n * sizeof(int);  // 计算 int 类型的字节大小
            double transferRate = dataSizeBytes / duration / 1e6; // MB/s
            std::cout << "Data size: " << dataSizeBytes << " bytes, Time: " << duration
                      << " seconds, Transfer rate: " << transferRate << " MB/s" << std::endl;
        }
    }
    
    MPI_Finalize();
    return 0;
}
