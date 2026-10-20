/* TWOWIN.C - start the BASIC and C drawing demos side by side */
int main(void)
{
	desk_run("C:\\DEMOS\\DRAW.BAS");
	os_sleep(1500);
	desk_run("C:\\DEMOS\\DRAW.C");
	return 0;
}
