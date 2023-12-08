Link to GitHub: https://github.com/jtfox142/OS-6/new/main?readme=1

This project simulates virtual memory in an operating system. Processes store addresses via pages in a frame table. Takes the same arguments as my projects 4 & 5. 
Example of how to run: ./oss -n 9 -s 8 -t 4 -f logfile.txt

For a little while, the project was experiencing segmentation faults but I do believe that I cured that (I was enqueueing pids into the fifoQueue rather than the frame number).
Even so, I added signal handling logic to properly terminate the program if it does happen again.

Same output goes to screen and logfile, which might be a little annoying. It is a lot.

The only thing that I know is missing from the project spec is that it doesn't terminate after 100 processes have entered the system. It will terminate after 5 real life seconds or if all processes naturally terminate.\
Speaking of, I was a bit confused about how we were supposed to make the child processes decide to terminate themselves, so I just left it up to random chance. After every 1000 or so memory requests, there is a 20% chance
of the child process terminating.

MADE BY JT FOX
DECEMBER 7, 2023
