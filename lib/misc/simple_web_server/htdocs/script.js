document.addEventListener('DOMContentLoaded', () => {
    fetch('/data.json')
        .then(response => {
            if (!response.ok) {
                throw new Error('Network response was not ok');
            }
            return response.json();
        })
        .then(data => {
            document.getElementById('message').textContent = data.message;
            const itemsList = document.getElementById('items-list');
            data.items.forEach(item => {
                const li = document.createElement('li');
                li.textContent = item;
                itemsList.appendChild(li);
            });
        })
        .catch(error => {
            console.error('There was a problem with the fetch operation:', error);
            document.getElementById('message').textContent = 'Error fetching data.';
        });
});
