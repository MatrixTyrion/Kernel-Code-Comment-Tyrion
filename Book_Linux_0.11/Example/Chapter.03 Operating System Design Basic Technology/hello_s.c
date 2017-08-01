/* hello_s.c */
#define		BLACK		 0
#define		BLUE         	 1
#define		GREEN        	 2
#define		CYAN         	 3
#define		RED          	 4
#define		MAGENTA      	 5
#define		BROWN        	 6
#define		LIGHTGRAY    	 7
#define		DARKGRAY     	 8
#define		LIGHTBLUE    	 9
#define		LIGHTGREEN   	10
#define		LIGHTCYAN    	11
#define		LIGHTRED     	12
#define		LIGHTMAGENTA 	13
#define		YELLOW       	14
#define		WHITE        	15
#define		BLINK        	28

void write_string(char *pstring, int color)
{
    char far *pvideo = (char far *)0xB8000000;
    while(*pstring)
    {
        *pvideo=*pstring;
        pstring++;
        pvideo++;
        *pvideo=color;
        pvideo++;
    }
}

void main(){
    write_string("Hello,world!", RED);
}
