#include <stdio.h>
#include <sys/time.h>
int main()
{
	int a, b, c;
	a = 0;
	b = 1;
	c = 3;
	char* tt = calloc(sizeof(int), 1);
	int* tmp_int = (int*)(void*)tt;
	int t = 10000;
	long long L1, L2;
	struct timeval tv;
	int cnt = 0;
	L1 = tv.tv_sec * 1000 * 1000 + tv.tv_usec;

	while (1 == 1)
	{
		cnt++;
		*tmp_int = (int)cnt;
		if (cnt >= 10000)
		{
			break;
		}
	}
	L2 = tv.tv_sec * 1000 * 1000 + tv.tv_usec;
	printf("%d ops  duration =  %lld  micro seconds \n", (a), L2 - L1);


}