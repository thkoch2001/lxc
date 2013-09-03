#!/bin/sh

_PRESEED_FILE="lxc-debconfig.cfg"

cat > "${_PRESEED_FILE}" << EOF
# lxc-debconfig ($(dpkg-parsechangelog -l../../changelog | awk '/^Version: / { print $2 }'))

lxc-debconfig lxc-debconfig/include-preseed-files string 
EOF

for _SCRIPT in $(ls ../templates/lxc-debconfig.d/????-* | grep -v '\.templates')
do
	_SCRIPT_NAME="$(basename ${_SCRIPT} | sed -e 's|^[0-9][0-9][0-9][0-9]-||')"

	for _DEBCONF in $(grep db_get ${_SCRIPT} | sed -e 's|.*db_get ||' -e 's|&&.*$||')
	do
		if ! grep -qs "lxc-debconfig ${_DEBCONF}" "${_PRESEED_FILE}"
		then
			if echo "${_DEBCONF}" | grep -qs '\{'
			then
				_COMMENT="#"
				_TYPE=""
			else
				_COMMENT=""
				_TYPE=" $(grep -A1 -m1 "^Template: ${_DEBCONF}" ../templates/lxc-debconfig.d/*-${_SCRIPT_NAME}.templates | awk '/^Type: / { print $2 }')"
			fi


			echo "${_COMMENT}lxc-debconfig ${_DEBCONF}${_TYPE} " >> "${_PRESEED_FILE}"
		fi
	done
done

if [ -e /srv/sources/live-systems/live-debconfig/examples/preseed.cfg ]
then
	echo "" >> "${_PRESEED_FILE}"
	echo "" >> "${_PRESEED_FILE}"
	cat /srv/sources/live-systems/live-debconfig/examples/preseed.cfg >> "${_PRESEED_FILE}"
else

cat >> "${_PRESEED_FILE}" << EOF

# Please see /usr/share/doc/live-debconfig/examples/preseed.cfg for system
# preseeding within the container.
EOF

fi
