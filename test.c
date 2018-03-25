#include <stdio.h>
#include <sys/time.h>
int main()
{
	int a, b, c;
	a = 0;
	b = 1;
	c = 3;
	char* tt = calloc(sizeof(int), 5);
	int arr[5] = {0, 1, 2, 3, 4};
	int* tmp_int = (int*)(void*)tt;
	int t = 10000;
	long long L1, L2;
	struct timeval tv;
	int cnt = 0;
	L1 = tv.tv_sec * 1000 * 1000 + tv.tv_usec;

	while (cnt <= 10000)
	{
		cnt++;
		memcpy(tmp_int, arr, sizeof(int) * 5);
	}
	L2 = tv.tv_sec * 1000 * 1000 + tv.tv_usec;
	printf("%d ops  duration =  %lld  micro seconds \n", (cnt), L2 - L1);


}