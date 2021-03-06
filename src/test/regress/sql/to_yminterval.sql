set grammar to oracle;
set datestyle='ISO,YMD';
select to_date('1-31-2016','MM-DD-yyyy') + to_yminterval('0-1')  as ndate from dual;
select to_date('1-31-2016','MM-DD-yyyy') - to_yminterval('1-1')  as ndate from dual;
select to_date('1-31-2016','MM-DD-yyyy') - to_yminterval('2016-1')  as ndate from dual;
select to_date('1-31-300','MM-DD-yyyy') - to_yminterval('320-0')  as ndate from dual;
select to_char(to_yminterval('2016-1'),'yyyy-mm')  as ndate from dual;
select to_date('1-31-2016','MM-DD-yyyy') - to_yminterval('')  as ndate from dual;
select to_date('1-31-2016','MM-DD-yyyy') + to_yminterval(null)  as ndate from dual;
select to_date('01-03-2016 10:30:00','MM-DD-yyyy hh:mi:ss') + to_yminterval('-2-02') as ndate from dual;
