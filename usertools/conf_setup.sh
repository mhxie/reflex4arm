cp ix.conf.sample ix.conf

SECOND_IP=`ifconfig ens6 | grep 'inet ' | awk '{print $2}'`
GATEWAY_IP=`ifconfig ens6 | grep 'inet ' | awk '{print $6}' | sed 's/255/1/g'`
sed -i "s/10.10.66.3/$SECOND_IP/g" ix.conf # update ip
sed -i "s/10.10.66.1/$GATEWAY_IP/g" ix.conf # update ip