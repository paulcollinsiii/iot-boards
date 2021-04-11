#!/bin/sh

kubectl get secret kaffi-homelab-ca -o jsonpath="{.data.ca\.crt}" | base64 -d - > ca.crt
vagrant ssh -- "sudo /bin/sh -c \"cp /vagrant/ca.crt /usr/local/share/ca-certificates && update-ca-certificates\""
