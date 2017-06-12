import pexpect

pexpect.run("rm data")
server = pexpect.spawn("./server")

clients = list();

for i in range(0,10):
  clients.append(pexpect.spawn("./client"))
  clients[i].expect("welcome")
print "clients created"

for i in range(0,3):
  clientfull = pexpect.spawn("./client")
  clientfull.expect("serverfull")
  clientfull.terminate(True)
print "tested max connections"

for i in range(0,10):
  clients[i].sendline("login "+str(i+1)) 
  clients[i].expect("ok")
print "tested login"

for i in range(0,10):
  c = clients[i];
  c.sendline("book ")
  c.expect("ok")
  c.sendline("book " + str(i))
  c.expect("available")
  c.sendline("book " + str(i) + " 02/07/2017")
  c.expect("done")

print "test booking 0"

c1 = clients[1]
c2 = clients[2]

c1.sendline("book")
c1.sendline("book 12")
c1.expect("available")
c2.sendline("book")
c2.sendline("book 12")
c2.expect("navailable")
c1.sendline("cancel")
c1.expect("ok")

print "test booking 1"

for i in range(0,10):
  c = clients[i];
  c.sendline("book")
  c.sendline("book 10")
  c.sendline("book 10 "+str(i+1)+"/08/2017 "+str(i+1)+"/08/2017")
  c.expect("done")

print "test booking 2"

server.terminate(True)
print "fine"
